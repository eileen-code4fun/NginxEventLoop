// g++ -o server server.cc
// ./server
#include <cerrno>
#include <unistd.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <signal.h>
#include <stdlib.h> 
#include <netinet/in.h>
#include <fcntl.h>
#include <string.h>

#define WORKER 4
#define PORT 6868
#define MAX_CONNS 100
#define MAX_EVENTS 1024
#define MAX_BUFFER_LEN 1024

static volatile bool running = true;

void abort(const char msg[]) {
  perror(msg);
  exit(1);
}

void registerFd(int efd, int fd) {
  struct epoll_event event;
  event.data.fd = fd;
  event.events = EPOLLIN | EPOLLET;
  if (epoll_ctl (efd, EPOLL_CTL_ADD, fd, &event) < 0) {
    abort("Failed to add FD to epoll");
  }
}

void setNonblocking(int fd) {
  int flags = fcntl (fd, F_GETFL, 0);
  if (flags == -1) {
    abort("Failed to obtain FD flags");
  }
  flags |= SOCK_NONBLOCK;
  if (fcntl (fd, F_SETFL, flags) < 0) {
    abort("Failed to set FD to nonblocking");
  }
}

void serve(pid_t pid, int sfd, struct sockaddr* addr, socklen_t* len) {
  int efd = epoll_create1 (0);
  if (efd < 0) {
    abort("Failed to create epoll event");
  }
  registerFd(efd, sfd);
  struct epoll_event* events = (struct epoll_event*)calloc(MAX_EVENTS, sizeof(struct epoll_event));
  while(running) {
    int n = epoll_wait(efd, events, MAX_EVENTS, 1000);
    for (int i = 0 ; i < n; i ++) {
      if (events[i].events & EPOLLERR || events[i].events & EPOLLHUP || !(events[i].events & EPOLLIN)) {
        // Error occurred. Ignore.
        continue;
      }
      if (events[i].data.fd == sfd) {
        // Events on listening socket.
        while(true) {
          // Accept until the queue is empty. Multiple listening events may occur for one epoll.
          int cfd = accept(sfd, addr, len);
          if (cfd == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Empty event on the listening queue.
            break;
          }
          if (cfd < 0) {
            abort("Failed to accept client socket");
          }
          setNonblocking(cfd);
          registerFd(efd, cfd);
        }
      } else {
        // Event on read.
        char buffer[512];
        int l = read(events[i].data.fd, buffer, MAX_BUFFER_LEN);
        buffer[l] = 0;
        printf("Pid: %d => %s\n", pid, buffer);
        // Done with the client socket. Close it to remove it from the epoll list.
        close(events[i].data.fd);
      }
    }
  }
  free(events);
}

void shutdown(int) {
  running = false;
}

int main(void)  {
  signal(SIGINT, shutdown);
  int sfd;
  struct sockaddr_in address;
  int addrlen = sizeof(address);
  // Creating socket file descriptor
  if ((sfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) == 0) {
    abort("Failed to create server socket");
  }
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(PORT);
  // Forcefully attaching socket to the port 8080
  if (bind(sfd, (struct sockaddr *)&address, sizeof(address))<0) {
    abort("Failed to bind server socket");
  }
  if (listen(sfd, MAX_CONNS) < 0) {
    abort("Failed to listen to server socket");
  }
  printf("Server started...\n");
  for (int i = 0; i < WORKER; i ++) {
    if (fork() == 0) {
      // Child process.
      pid_t pid = getpid();
      printf("Worker %d is ready\n", pid);
      serve(pid, sfd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
      printf("Worker %d shuts down\n", pid);
      return 0;
    }
  }
  while(running) {
    sleep(1);
  }
  close(sfd);
  printf("Gracefully exiting the program\n");
  return 0;
}
