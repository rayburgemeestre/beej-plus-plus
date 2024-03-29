#include "beej.h"

#include <sstream>
#include <iostream>

namespace {
// Get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa) {
  if (sa->sa_family == AF_INET) {
    return &(((struct sockaddr_in *)sa)->sin_addr);
  }

  return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

// Return a listening socket
int get_listener_socket(int port) {
  int listener;  // Listening socket descriptor
  int yes = 1;   // For setsockopt() SO_REUSEADDR, below
  int rv;

  struct addrinfo hints, *ai, *p;

  // Get us a socket and bind it
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  if ((rv = getaddrinfo(NULL, std::to_string(port).c_str(), &hints, &ai)) != 0) {
    fprintf(stderr, "selectserver: %s\n", gai_strerror(rv));
    exit(1);
  }

  for (p = ai; p != NULL; p = p->ai_next) {
    listener = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (listener < 0) {
      continue;
    }

    // Lose the pesky "address already in use" error message
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

    if (bind(listener, p->ai_addr, p->ai_addrlen) < 0) {
      close(listener);
      continue;
    }

    break;
  }

  freeaddrinfo(ai);  // All done with this

  // If we got here, it means we didn't get bound
  if (p == NULL) {
    return -1;
  }

  // Listen
  if (listen(listener, 10) == -1) {
    return -1;
  }

  return listener;
}

// Add a new file descriptor to the set
void add_to_pfds(struct pollfd *pfds[], int newfd, int *fd_count, int *fd_size) {
  // If we don't have room, add more space in the pfds array
  if (*fd_count == *fd_size) {
    *fd_size *= 2;  // Double it

    *pfds = (pollfd *)realloc(*pfds, sizeof(**pfds) * (*fd_size));
  }

  (*pfds)[*fd_count].fd = newfd;
  (*pfds)[*fd_count].events = POLLIN;  // Check ready-to-read

  (*fd_count)++;
}

// Remove an index from the set
void del_from_pfds(struct pollfd pfds[], int i, int *fd_count) {
  // Copy the one from the end over this one
  pfds[i] = pfds[*fd_count - 1];

  (*fd_count)--;
}

int sendall(int s, char *buf, int *len) {
  int total = 0;         // how many bytes we've sent
  int bytesleft = *len;  // how many we have left to send
  int n;

  while (total < *len) {
    n = send(s, buf + total, bytesleft, 0);
    if (n == -1) {
      break;
    }
    total += n;
    bytesleft -= n;
  }

  *len = total;  // return number actually sent here

  return n == -1 ? -1 : 0;  // return -1 on failure, 0 on success
}

}  // namespace

namespace beej {

server::server(int port) : port(port) {}

void server::run() {
  pfds = (struct pollfd *)malloc(sizeof *pfds * fd_size);

  // Set up and get a listening socket
  listener = get_listener_socket(port);

  if (listener == -1) {
    fprintf(stderr, "error getting listening socket\n");
    exit(1);
  }

  // Add the listener to set
  pfds[0].fd = listener;
  pfds[0].events = POLLIN;  // Report ready to read on incoming connection

  fd_count = 1;  // For the listener

  // Main loop
  for (;;) {
    int poll_count = poll(pfds, fd_count, -1);

    if (poll_count == -1) {
      perror("poll");
      exit(1);
    }

    // Run through the existing connections looking for data to read
    for (int i = 0; i < fd_count; i++) {
      // Check if someone's ready to read
      if (pfds[i].revents & POLLIN) {  // We got one!!

        if (pfds[i].fd == listener) {
          // If listener is ready to read, handle new connection

          addrlen = sizeof remoteaddr;
          newfd = accept(listener, (struct sockaddr *)&remoteaddr, &addrlen);

          if (newfd == -1) {
            perror("accept");
          } else {
            add_to_pfds(&pfds, newfd, &fd_count, &fd_size);
          }
        } else {
          // If not the listener, we're just a regular client
          int nbytes = recv(pfds[i].fd, buf, sizeof buf, 0);

          int sender_fd = pfds[i].fd;

          if (nbytes <= 0) {
            // Got error or connection closed by client
            if (nbytes == 0) {
              // Connection closed
            } else {
              perror("recv");
            }

            close(pfds[i].fd);  // Bye!

            del_from_pfds(pfds, i, &fd_count);

          } else {
            buffers[sender_fd].append(buf, nbytes);
            while (buffers[sender_fd].has_line()) {
              auto p = callback(buffers[sender_fd].get_line());
              bool disconnect = p.first;
              std::string s = p.second;
              std::stringstream ss(s);
              std::string line;
              while (std::getline(ss, line, '\n')) {
                  line = line + "\r\n";
                  int len = line.size();
                  // std::cout << "Sending back: " << line << std::endl;
                  sendall(sender_fd, (char *)line.c_str(), &len);
              }
              if (disconnect) close(sender_fd);
            }
          }
        }
      }
    }
  }
}

void server::on_line(std::function<std::pair<bool,std::string>(const std::string &line)> callback) {
  this->callback = callback;
}

client::client(std::string hostname, int port) : hostname(std::move(hostname)), port(port) {}

client::~client() {
  disconnect();
}

void client::connect() {
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  if ((rv = getaddrinfo(hostname.c_str(), std::to_string(port).c_str(), &hints, &servinfo)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    return;
  }

  // loop through all the results and connect to the first we can
  for (p = servinfo; p != NULL; p = p->ai_next) {
    if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
      perror("client: socket");
      continue;
    }

    if (::connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
      close(sockfd);
      perror("client: connect");
      continue;
    }

    break;
  }

  if (p == NULL) {
    fprintf(stderr, "client: failed to connect\n");
    return;
  }

  inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr), s, sizeof s);

  freeaddrinfo(servinfo);  // all done with this structure
}

void client::disconnect() {
  close(sockfd);
}

void client::send(const std::string &data) {
  int len = data.size();
  sendall(sockfd, (char *)data.c_str(), &len);
}

void client::read(std::function<void(const std::string&)> callback) {
    while (true) {
        int nbytes = recv(sockfd, buf, sizeof buf, 0);
        if (nbytes <= 0) {
            if (nbytes != 0) {
                // perror("recv");
            }
            disconnect();
            return;
        } else {
            buffer.append(buf, nbytes);
            std::string line;
            while (buffer.has_line()) {
                line = buffer.get_line();
                line.erase(line.find_last_not_of(" \n\r\t") + 1);
                if (!line.empty()) callback(line);
            }
        }
    }
}

}  // namespace beej
