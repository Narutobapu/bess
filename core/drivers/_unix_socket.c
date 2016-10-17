#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../port.h"

#define NOT_CONNECTED -1

/* Only one client can be connected at the same time */

/* Polling sockets is quite exprensive, so we throttle the polling rate.
 * (by checking sockets once every RECV_TICKS schedules)
 * TODO: Revise this once the interrupt mode is implemented */
#define RECV_SKIP_TICKS 256

#define MAX_TX_FRAGS 8

class UnixSocketPort : public Port {
 public:
  static void InitDriver(){};

  virtual struct snobj *Init(struct snobj *arg);
  virtual void DeInit();

  virtual int RecvPackets(queue_t qid, snb_array_t pkts, int cnt);
  virtual int SendPackets(queue_t qid, snb_array_t pkts, int cnt);

  void AcceptNewClient();

 private:
  void CloseConnection();

  uint32_t recv_skip_cnt_ = {0};
  int listen_fd_ = {0};
  struct sockaddr_un addr_ = {0};

  /* NOTE: three threads (accept / recv / send) may race on this,
   * so use volatile */
  volatile int client_fd_ = {0};
  int old_client_fd_ = {0};

  pthread_t accept_thread_ = {0};
};

void UnixSocketPort::AcceptNewClient() {
  int ret;

  for (;;) {
    ret = accept4(this->listen_fd_, NULL, NULL, SOCK_NONBLOCK);
    if (ret >= 0) break;

    if (errno != EINTR) log_perr("[UnixSocket]:accept4()");
  }

  this->recv_skip_cnt_ = 0;

  if (this->old_client_fd_ != NOT_CONNECTED) {
    /* Reuse the old file descriptor number by atomically
     * exchanging the new fd with the old one.
     * The zombie socket is closed silently (see dup2) */
    dup2(ret, this->client_fd_);
    close(ret);
  } else
    this->client_fd_ = ret;
}

/* This accept thread terminates once a new client is connected */
void *AcceptThreadMain(void *arg) {
  UnixSocketPort *p = reinterpret_cast<UnixSocketPort *>(arg);
  pthread_detach(pthread_self());
  p->AcceptNewClient();
  return NULL;
}

/* The file descriptor for the connection will not be closed,
 * until we have a new client. This is to avoid race condition in TX process */
void UnixSocketPort::CloseConnection() {
  int ret;

  /* Keep client_fd, since it may be being used in unix_send_pkts() */
  this->old_client_fd_ = this->client_fd_;
  this->client_fd_ = NOT_CONNECTED;

  /* relaunch the accept thread */
  ret = pthread_create(&this->accept_thread_, NULL, AcceptThreadMain,
                       reinterpret_cast<void *>(this));
  this->accept_thread_ = 0;
  if (ret) log_err("[UnixSocket]:pthread_create() returned errno %d", ret);
}

struct snobj *UnixSocketPort::Init(struct snobj *conf) {
  int num_txq = this->num_queues[PACKET_DIR_OUT];
  int num_rxq = this->num_queues[PACKET_DIR_INC];

  const char *path;
  size_t addrlen;

  int ret;

  this->client_fd_ = NOT_CONNECTED;
  this->old_client_fd_ = NOT_CONNECTED;

  if (num_txq > 1 || num_rxq > 1)
    return snobj_err(EINVAL, "Cannot have more than 1 queue per RX/TX");

  this->listen_fd_ = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  if (this->listen_fd_ < 0) return snobj_err(errno, "socket(AF_UNIX) failed");

  this->addr_.sun_family = AF_UNIX;

  path = snobj_eval_str(conf, "path");
  if (path) {
    snprintf(this->addr_.sun_path, sizeof(this->addr_.sun_path), "%s", path);
  } else
    snprintf(this->addr_.sun_path, sizeof(this->addr_.sun_path),
             "%s/bess_unix_%s", P_tmpdir, this->Name().c_str());

  /* This doesn't include the trailing null character */
  addrlen = sizeof(this->addr_.sun_family) + strlen(this->addr_.sun_path);

  /* non-abstract socket address? */
  if (this->addr_.sun_path[0] != '@') {
    /* remove existing socket file, if any */
    unlink(this->addr_.sun_path);
  } else
    this->addr_.sun_path[0] = '\0';

  ret = bind(this->listen_fd_,
             reinterpret_cast<struct sockaddr *>(&this->addr_), addrlen);
  if (ret < 0) return snobj_err(errno, "bind(%s) failed", this->addr_.sun_path);

  ret = listen(this->listen_fd_, 1);
  if (ret < 0) return snobj_err(errno, "listen() failed");

  ret = pthread_create(&this->accept_thread_, NULL, AcceptThreadMain,
                       reinterpret_cast<void *>(this));
  this->accept_thread_ = 0;
  if (ret) return snobj_err(ret, "pthread_create() failed");

  return NULL;
}

void UnixSocketPort::DeInit() {
  if (this->accept_thread_) pthread_cancel(this->accept_thread_);

  close(this->listen_fd_);

  if (this->client_fd_ >= 0) close(this->client_fd_);
}

int UnixSocketPort::RecvPackets(queue_t qid, snb_array_t pkts, int cnt) {
  int client_fd = this->client_fd_;

  int received;

  if (client_fd == NOT_CONNECTED) return 0;

  if (this->recv_skip_cnt_) {
    this->recv_skip_cnt_--;
    return 0;
  }

  received = 0;
  while (received < cnt) {
    struct snbuf *pkt = static_cast<struct snbuf *>(snb_alloc());
    int ret;

    if (!pkt) break;

    /* datagrams larger than 2KB will be truncated */
    ret = recv(client_fd, pkt->_data, SNBUF_DATA, 0);

    if (ret > 0) {
      snb_append(pkt, ret);
      pkts[received++] = pkt;
      continue;
    }

    snb_free(pkt);

    if (ret < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;

      if (errno == EINTR) continue;
    }

    /* connection closed */
    this->CloseConnection();
    break;
  }

  if (received == 0) this->recv_skip_cnt_ = RECV_SKIP_TICKS;

  return received;
}

int UnixSocketPort::SendPackets(queue_t qid, snb_array_t pkts, int cnt) {
  int client_fd = this->client_fd_;
  int sent = 0;

  for (int i = 0; i < cnt; i++) {
    struct snbuf *pkt = pkts[i];
    struct rte_mbuf *mbuf = &pkt->mbuf;

    int nb_segs = mbuf->nb_segs;
    struct iovec iov[nb_segs];

    struct msghdr msg;
    msg.msg_iov = iov;
    msg.msg_iovlen = nb_segs;

    ssize_t ret;

    for (int j = 0; j < nb_segs; j++) {
      iov[j].iov_base = rte_pktmbuf_mtod(mbuf, void *);
      iov[j].iov_len = rte_pktmbuf_data_len(mbuf);
      mbuf = mbuf->next;
    }

    ret = sendmsg(client_fd, &msg, 0);
    if (ret < 0) break;

    sent++;
  }

  if (sent) snb_free_bulk(pkts, sent);

  return sent;
}

ADD_DRIVER(UnixSocketPort, "unix_port",
           "packet exchange via a UNIX domain socket")