#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 6
#define SCREEN_WIDTH (int)(1024 * 1.2)
#define SCREEN_HEIGHT (int)(640 * 1.2)
#define WORM_SPEED 2.0
#define SPEED_BOOST_MULTIPLIER 3
#define TURN_SPEED 0.1
#define WORM_RADIUS 3
#define MAX_PATH_LENGTH 150
#define INPUT_BUFFER_SIZE 10
#define PI 3.14159265
#define MAX_BULLETS 3
#define BULLET_SPEED 12
#define POWERUP_SPAWN_INTERVAL 5
#define MAX_POWERUPS 3
#define SPAWN_CIRCLE_RADIUS 50
#define POWERUP_RADIUS 10
#define BULLET_RADIUS 5
#define TAIL_COLLISION_THRESHOLD 10
#define INVINCIIBILITY_TIME 2
#define SPEED_BOOST_DURATION 3.0
#define BULLET_COOLDOWN 0.003

typedef struct {
  float x;
  float y;
} Point;

typedef struct {
  Point position;
  float angle;
  bool active;
} Bullet;

typedef enum { POWERUP_BULLETS, POWERUP_SPEED } PowerupType;

typedef struct {
  Point position;
  PowerupType type;
  bool active;
} Powerup;

typedef struct {
  bool left;
  bool right;
  bool up;
} InputState;

typedef struct {
  Point position;
  float angle;
  bool alive;
  InputState input;
  pthread_mutex_t input_mutex;
  Point path[MAX_PATH_LENGTH];
  int path_length;
  int bullets_left;
  Bullet bullets[MAX_BULLETS];
  time_t invincibility_end;
  float speed_boost_time_left;
  bool speed_boost_active;
  float last_shot_time; // New variable to track the last shot time
} Worm;

typedef struct {
  int socket;
  Worm worm;
} Client;

Client clients[MAX_CLIENTS];
int num_clients = 0;
bool game_started = false;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
Powerup powerups[MAX_POWERUPS];
int active_powerups = 0;
time_t last_powerup_spawn = 0;

void initWorm(Worm *worm, float startX, float startY, float angle) {
  worm->position.x = startX;
  worm->position.y = startY;
  worm->angle = angle;
  worm->alive = true;
  pthread_mutex_init(&worm->input_mutex, NULL);
  worm->path[0] = (Point){startX, startY};
  worm->path_length = 1;
  worm->bullets_left = 0;
  for (int i = 0; i < MAX_BULLETS; i++) {
    worm->bullets[i].active = false;
  }
  worm->invincibility_end = time(NULL) + INVINCIIBILITY_TIME;
  worm->speed_boost_time_left = 0;
  worm->speed_boost_active = false;
  worm->input.left = false;
  worm->input.right = false;
  worm->input.up = false;
  worm->last_shot_time = 0;
  memset(&worm->input, 0, sizeof(InputState));
}

void addPointToPath(Worm *worm, Point newPoint) {
  if (worm->path_length < MAX_PATH_LENGTH) {
    worm->path[worm->path_length++] = newPoint;
  } else {
    for (int i = 1; i < MAX_PATH_LENGTH; i++) {
      worm->path[i - 1] = worm->path[i];
    }
    worm->path[MAX_PATH_LENGTH - 1] = newPoint;
  }
}

bool checkTailCollision(Worm *worm, Point newPosition) {
  int start = (worm->path_length > TAIL_COLLISION_THRESHOLD)
                  ? worm->path_length - TAIL_COLLISION_THRESHOLD
                  : 0;

  for (int i = 0; i < start; i++) {
    float dx = newPosition.x - worm->path[i].x;
    float dy = newPosition.y - worm->path[i].y;
    float distance = sqrt(dx * dx + dy * dy);
    if (distance < WORM_RADIUS * 2) {
      return true;
    }
  }
  return false;
}

bool checkCollision(Worm *worm, Point newPosition) {
  if (time(NULL) < worm->invincibility_end) {
    return false;
  }

  if (checkTailCollision(worm, newPosition)) {
    return true;
  }

  // Check collision with other worms
  for (int i = 0; i < num_clients; i++) {
    Worm *otherWorm = &clients[i].worm;
    if (otherWorm == worm || !otherWorm->alive)
      continue;

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

void spawnPowerup() {
  if (active_powerups < MAX_POWERUPS) {
    Powerup new_powerup;
    new_powerup.position.x = rand() % SCREEN_WIDTH;
    new_powerup.position.y = rand() % SCREEN_HEIGHT;
    new_powerup.active = true;
    new_powerup.type = (rand() % 2 == 0) ? POWERUP_BULLETS : POWERUP_SPEED;
    powerups[active_powerups++] = new_powerup;
    printf("Spawned powerup of type: %d at position (%.2f, %.2f)\n",
           new_powerup.type, new_powerup.position.x, new_powerup.position.y);
  }
}

void updateBullets(Worm *worm) {
  for (int i = 0; i < MAX_BULLETS; i++) {
    if (worm->bullets[i].active) {
      worm->bullets[i].position.x += cos(worm->bullets[i].angle) * BULLET_SPEED;
      worm->bullets[i].position.y += sin(worm->bullets[i].angle) * BULLET_SPEED;

      // Check if bullet is out of bounds
      if (worm->bullets[i].position.x < 0 ||
          worm->bullets[i].position.x > SCREEN_WIDTH ||
          worm->bullets[i].position.y < 0 ||
          worm->bullets[i].position.y > SCREEN_HEIGHT) {
        worm->bullets[i].active = false;
      }
    }
  }
}

bool checkBulletCollision(Point bullet_pos, Worm *target_worm) {
  float dx = bullet_pos.x - target_worm->position.x;
  float dy = bullet_pos.y - target_worm->position.y;
  float distance = sqrt(dx * dx + dy * dy);
  return distance < (WORM_RADIUS + BULLET_RADIUS);
}

void updateWorm(int clientIndex) {
  Worm *worm = &clients[clientIndex].worm;
  if (!worm->alive)
    return;

  pthread_mutex_lock(&worm->input_mutex);
  InputState input = worm->input;
  pthread_mutex_unlock(&worm->input_mutex);

  if (input.left) {
    worm->angle -= TURN_SPEED;
  }
  if (input.right) {
    worm->angle += TURN_SPEED;
  }

  float current_time = (float)clock() / CLOCKS_PER_SEC;
  float current_speed = WORM_SPEED;
  if (input.up) {
    if (worm->speed_boost_time_left > 0) {
      current_speed *= SPEED_BOOST_MULTIPLIER;
      worm->speed_boost_time_left -= 1.0 / 60.0; // Assuming 60 FPS
      if (worm->speed_boost_time_left <= 0) {
        worm->speed_boost_time_left = 0;
      }
    } else if (worm->bullets_left > 0 &&
               (current_time - worm->last_shot_time) >= BULLET_COOLDOWN) {
      // Shoot a bullet
      for (int i = 0; i < MAX_BULLETS; i++) {
        if (!worm->bullets[i].active) {
          worm->bullets[i].position = worm->position;
          worm->bullets[i].angle = worm->angle;
          worm->bullets[i].active = true;
          worm->bullets_left--;
          worm->last_shot_time = current_time;
          printf("Bullet fired by worm %d. Bullets left: %d\n", clientIndex,
                 worm->bullets_left);
          break;
        }
      }
    }
  }

  Point newPosition = {worm->position.x + cos(worm->angle) * current_speed,
                       worm->position.y + sin(worm->angle) * current_speed};

  newPosition.x = fmod(newPosition.x + SCREEN_WIDTH, SCREEN_WIDTH);
  newPosition.y = fmod(newPosition.y + SCREEN_HEIGHT, SCREEN_HEIGHT);

  if (checkCollision(worm, newPosition)) {
    worm->alive = false;
    if (checkTailCollision(worm, newPosition)) {
      printf("Worm %d collided with its own tail!\n", clientIndex);
    } else {
      printf("Worm %d collided and died!\n", clientIndex);
    }
  } else {
    worm->position = newPosition;
    addPointToPath(worm, newPosition);

    for (int i = 0; i < active_powerups; i++) {
      if (powerups[i].active) {
        float dx = worm->position.x - powerups[i].position.x;
        float dy = worm->position.y - powerups[i].position.y;
        float distance = sqrt(dx * dx + dy * dy);
        if (distance < POWERUP_RADIUS + WORM_RADIUS) {
          if (powerups[i].type == POWERUP_BULLETS) {
            worm->bullets_left = 3;
            worm->speed_boost_time_left = 0;
            worm->speed_boost_active = false;
          } else if (powerups[i].type == POWERUP_SPEED) {
            worm->speed_boost_time_left = SPEED_BOOST_DURATION;
            worm->bullets_left = 0;
          }
          powerups[i].active = false;
          // Move last active powerup to this slot and decrease count
          powerups[i] = powerups[--active_powerups];
          printf("Worm %d collected a powerup! Type: %d\n", clientIndex,
                 powerups[i].type);
        }
      }
    }
  }

  updateBullets(worm);

  for (int i = 0; i < MAX_BULLETS; i++) {
    if (worm->bullets[i].active) {
      for (int j = 0; j < num_clients; j++) {
        if (j != clientIndex && clients[j].worm.alive) {
          if (checkBulletCollision(worm->bullets[i].position,
                                   &clients[j].worm)) {
            clients[j].worm.alive = false;
            worm->bullets[i].active = false;
            printf("Worm %d shot and killed worm %d!\n", clientIndex, j);
          }
        }
      }
    }
  }
}

void handle_client(int client_socket) {
  char buffer[8192];
  int n;

  while ((n = recv(client_socket, buffer, sizeof(buffer), 0)) > 0) {
    buffer[n] = '\0';
    if (strncmp(buffer, "JOIN", 4) == 0) {
      pthread_mutex_lock(&clients_mutex);
      if (num_clients < MAX_CLIENTS) {
        clients[num_clients].socket = client_socket;

        float angle = (2 * PI * num_clients) / MAX_CLIENTS;
        float startX = SCREEN_WIDTH / 2.0 + SPAWN_CIRCLE_RADIUS * cos(angle);
        float startY = SCREEN_HEIGHT / 2.0 + SPAWN_CIRCLE_RADIUS * sin(angle);
        float spawnAngle = angle;
        spawnAngle += ((rand() % 200) / 100.0) - 1.0;

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
    } else if (strncmp(buffer, "INPUT", 5) == 0) {
      for (int i = 0; i < num_clients; i++) {
        if (clients[i].socket == client_socket) {
          pthread_mutex_lock(&clients[i].worm.input_mutex);
          sscanf(buffer, "INPUT %d %d %d", &clients[i].worm.input.left,
                 &clients[i].worm.input.right, &clients[i].worm.input.up);
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

      // Spawn powerups
      time_t current_time = time(NULL);
      if (current_time - last_powerup_spawn >= POWERUP_SPAWN_INTERVAL) {
        spawnPowerup();
        last_powerup_spawn = current_time;
      }

      for (int i = 0; i < num_clients; i++) {
        updateWorm(i);
      }

      char state[16384];
      int offset = 0;
      offset += snprintf(state + offset, sizeof(state) - offset, "STATE %d ",
                         num_clients);

      // Add powerup information
      offset += snprintf(state + offset, sizeof(state) - offset, "%d ",
                         active_powerups);
      for (int i = 0; i < active_powerups; i++) {
        offset += snprintf(state + offset, sizeof(state) - offset,
                           "%.2f %.2f %d ", powerups[i].position.x,
                           powerups[i].position.y, powerups[i].type);
      }

      for (int i = 0; i < num_clients; i++) {
        Worm *worm = &clients[i].worm;
        offset += snprintf(state + offset, sizeof(state) - offset,
                           "%d %.2f %.2f %.2f %d %d %.2f %d ",
                           worm->path_length, worm->position.x,
                           worm->position.y, worm->angle, worm->alive ? 1 : 0,
                           worm->bullets_left, worm->speed_boost_time_left,
                           worm->speed_boost_active ? 1 : 0);

        // Add bullet information
        for (int j = 0; j < MAX_BULLETS; j++) {
          if (worm->bullets[j].active) {
            offset +=
                snprintf(state + offset, sizeof(state) - offset,
                         "%.2f %.2f %.2f ", worm->bullets[j].position.x,
                         worm->bullets[j].position.y, worm->bullets[j].angle);
          } else {
            offset +=
                snprintf(state + offset, sizeof(state) - offset, "0 0 0 ");
          }
        }

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
  srand(time(NULL));
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
