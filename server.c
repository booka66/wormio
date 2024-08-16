#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 6

#define SCREEN_WIDTH 1024
#define SCREEN_HEIGHT 640

#define WORM_SPEED 2.0
#define TURN_SPEED 0.1
#define WORM_RADIUS 3
#define MAX_PATH_LENGTH 100
#define MAX_PATH_SEND 200

#define SPAWN_CIRCLE_RADIUS 50

#define INPUT_BUFFER_SIZE 10
#define PI 3.14159265

typedef struct {
  float x;
  float y;
} Point;

typedef struct {
  Point position;
  float angle;
  bool alive;
  char input_buffer[INPUT_BUFFER_SIZE][10];
  int input_buffer_count;
  pthread_mutex_t input_mutex;
  Point path[MAX_PATH_LENGTH];
  int path_length;
} Worm;

typedef struct {
  int socket;
  Worm worm;
  int grace_ticks;
} Client;

Client clients[MAX_CLIENTS];
int num_clients = 0;
bool game_started = false;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

void initWorm(Worm *worm, float startX, float startY, float angle) {
  worm->position.x = startX;
  worm->position.y = startY;
  worm->angle = angle;
  worm->alive = true;
  worm->input_buffer_count = 0;
  pthread_mutex_init(&worm->input_mutex, NULL);
  worm->path[0] = (Point){startX, startY};
  worm->path_length = 1;
}

void addPointToPath(Worm *worm, Point newPoint) {
  if (worm->path_length < MAX_PATH_LENGTH) {
    worm->path[worm->path_length++] = newPoint;
  } else {
    // Shift the path array to make room for the new point
    for (int i = 1; i < MAX_PATH_LENGTH; i++) {
      worm->path[i - 1] = worm->path[i];
    }
    worm->path[MAX_PATH_LENGTH - 1] = newPoint;
  }
}

bool checkCollision(Worm *worm, Point newPosition, int currentClient) {
  for (int i = 0; i < num_clients; i++) {
    if (i == currentClient)
      continue; // Don't collide with self
    Worm *otherWorm = &clients[i].worm;
    for (int j = 0; j < otherWorm->path_length; j++) {
      float dx = newPosition.x - otherWorm->path[j].x;
      float dy = newPosition.y - otherWorm->path[j].y;
      float distance = sqrt(dx * dx + dy * dy);
      if (distance < WORM_RADIUS * 2) {
        return true;
      }
    }
  }
  return false;
}

void updateWorm(int clientIndex) {
  Worm *worm = &clients[clientIndex].worm;
  if (!worm->alive)
    return;

  pthread_mutex_lock(&worm->input_mutex);
  if (worm->input_buffer_count > 0) {
    if (strcmp(worm->input_buffer[0], "LEFT") == 0) {
      worm->angle -= TURN_SPEED;
    } else if (strcmp(worm->input_buffer[0], "RIGHT") == 0) {
      worm->angle += TURN_SPEED;
    }
    for (int i = 1; i < worm->input_buffer_count; i++) {
      strcpy(worm->input_buffer[i - 1], worm->input_buffer[i]);
    }
    worm->input_buffer_count--;
  }
  pthread_mutex_unlock(&worm->input_mutex);

  Point newPosition = {worm->position.x + cos(worm->angle) * WORM_SPEED,
                       worm->position.y + sin(worm->angle) * WORM_SPEED};

  // Wrap around screen edges
  if (newPosition.x < 0)
    newPosition.x = SCREEN_WIDTH;
  if (newPosition.x > SCREEN_WIDTH)
    newPosition.x = 0;
  if (newPosition.y < 0)
    newPosition.y = SCREEN_HEIGHT;
  if (newPosition.y > SCREEN_HEIGHT)
    newPosition.y = 0;

  if (!checkCollision(worm, newPosition, clientIndex)) {
    worm->position = newPosition;
    addPointToPath(worm, newPosition);
  } else {
    worm->alive = false;
  }
}

void handle_client(int client_socket) {
  char buffer[1024];
  int n;

  while ((n = recv(client_socket, buffer, sizeof(buffer), 0)) > 0) {
    buffer[n] = '\0';
    if (strncmp(buffer, "JOIN", 4) == 0) {
      pthread_mutex_lock(&clients_mutex);
      if (num_clients < MAX_CLIENTS) {
        clients[num_clients].socket = client_socket;

        // Calculate angle and spawn position for the new worm
        float angle = (2 * PI * num_clients) / MAX_CLIENTS;
        float startX = SCREEN_WIDTH / 2.0 + SPAWN_CIRCLE_RADIUS * cos(angle);
        float startY = SCREEN_HEIGHT / 2.0 + SPAWN_CIRCLE_RADIUS * sin(angle);
        float spawnAngle = angle; // Face outward
        // Add random offset to angle
        spawnAngle += (rand() % 100) / 100.0 - 0.5;

        initWorm(&clients[num_clients].worm, startX, startY, spawnAngle);

        char id_msg[20];
        snprintf(id_msg, sizeof(id_msg), "PLAYER_ID %d", num_clients);
        send(client_socket, id_msg, strlen(id_msg), 0);

        num_clients++;
        printf("New client joined. Total clients: %d\n", num_clients);
      } else {
        const char *msg = "Server full";
        send(client_socket, msg, strlen(msg), 0);
        close(client_socket);
        pthread_mutex_unlock(&clients_mutex);
        return;
      }
      pthread_mutex_unlock(&clients_mutex);
    } else if (strncmp(buffer, "START", 5) == 0) {
      if (!game_started && num_clients > 0) {
        game_started = true;
        printf("Game started!\n");
        const char *msg = "GAME_STARTED";
        for (int i = 0; i < num_clients; i++) {
          send(clients[i].socket, msg, strlen(msg), 0);
        }
      }
    } else if (strncmp(buffer, "LEFT", 4) == 0 ||
               strncmp(buffer, "RIGHT", 5) == 0) {
      for (int i = 0; i < num_clients; i++) {
        if (clients[i].socket == client_socket) {
          pthread_mutex_lock(&clients[i].worm.input_mutex);
          if (clients[i].worm.input_buffer_count < INPUT_BUFFER_SIZE) {
            strncpy(clients[i]
                        .worm.input_buffer[clients[i].worm.input_buffer_count],
                    buffer, sizeof(clients[i].worm.input_buffer[0]) - 1);
            clients[i]
                .worm
                .input_buffer[clients[i].worm.input_buffer_count]
                             [sizeof(clients[i].worm.input_buffer[0]) - 1] =
                '\0';
            clients[i].worm.input_buffer_count++;
          }
          pthread_mutex_unlock(&clients[i].worm.input_mutex);
          break;
        }
      }
    }
  }

  pthread_mutex_lock(&clients_mutex);
  for (int i = 0; i < num_clients; i++) {
    if (clients[i].socket == client_socket) {
      for (int j = i; j < num_clients - 1; j++) {
        clients[j] = clients[j + 1];
      }
      num_clients--;
      break;
    }
  }
  pthread_mutex_unlock(&clients_mutex);
  close(client_socket);
  printf("Client disconnected. Total clients: %d\n", num_clients);
}

void game_loop() {
  while (1) {
    if (game_started && num_clients > 0) {
      pthread_mutex_lock(&clients_mutex);
      for (int i = 0; i < num_clients; i++) {
        updateWorm(i);
      }

      char state[16384];
      int offset = 0;
      offset += snprintf(state + offset, sizeof(state) - offset, "STATE %d ",
                         num_clients);
      for (int i = 0; i < num_clients; i++) {
        Worm *worm = &clients[i].worm;
        offset += snprintf(state + offset, sizeof(state) - offset,
                           "%d %.2f %.2f %.2f %d ", worm->path_length,
                           worm->position.x, worm->position.y, worm->angle,
                           worm->alive ? 1 : 0);

        // Add all path points
        for (int j = 0; j < worm->path_length; j++) {
          offset += snprintf(state + offset, sizeof(state) - offset,
                             "%.2f %.2f ", worm->path[j].x, worm->path[j].y);
        }

        if (offset >= (int)sizeof(state) - 1) {
          fprintf(stderr, "State message truncated\n");
          break;
        }
      }
      for (int i = 0; i < num_clients; i++) {
        send(clients[i].socket, state, strlen(state), 0);
      }

      pthread_mutex_unlock(&clients_mutex);
    }
    usleep(16667); // ~60 FPS
  }
}

int main() {
  int server_fd, new_socket;
  struct sockaddr_in address;
  int opt = 1;
  int addrlen = sizeof(address);

  if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }

  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  }

  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(8080);

  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    perror("bind failed");
    exit(EXIT_FAILURE);
  }

  if (listen(server_fd, 3) < 0) {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  printf("Server listening on port 8080\n");

  pthread_t game_thread;
  pthread_create(&game_thread, NULL, (void *(*)(void *))game_loop, NULL);

  while (1) {
    if ((new_socket = accept(server_fd, (struct sockaddr *)&address,
                             (socklen_t *)&addrlen)) < 0) {
      perror("accept");
      continue;
    }

    printf("New connection accepted\n");

    pthread_t client_thread;
    pthread_create(&client_thread, NULL, (void *(*)(void *))handle_client,
                   (void *)(intptr_t)new_socket);
  }

  return 0;
}
