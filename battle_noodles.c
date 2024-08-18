#include <SDL.h>
#include <arpa/inet.h>
#include <errno.h>
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
#define TURN_SPEED 0.1
#define WORM_RADIUS 3
#define INPUT_BUFFER_SIZE 10
#define MAX_BULLETS 3
#define BULLET_RADIUS 5
#define MAX_POWERUPS 3
#define POWERUP_RADIUS 10
#define CLIENT_TICK_RATE 60
#define SERVER_PORT 8080
#define DISCOVERY_PORT 8081
#define MAX_SERVERS 10
#define BULLET_SPEED 12
#define SPAWN_CIRCLE_RADIUS 50
#define TAIL_COLLISION_THRESHOLD 10
#define INVINCIIBILITY_TIME 2
#define SPEED_BOOST_DURATION 3.0
#define SPEED_BOOST_MULTIPLIER 3.0
#define BULLET_COOLDOWN 0.5
#define PI 3.14159265358979323846

typedef struct {
  float x;
  float y;
} Point;

typedef struct {
  Point position;
  float angle;
  bool active;
} Bullet;

typedef struct {
  bool left;
  bool right;
  bool up;
} InputState;

typedef enum { POWERUP_BULLETS, POWERUP_SPEED, POWERUP_GHOST } PowerupType;

typedef struct {
  Point position;
  PowerupType type;
} Powerup;

typedef struct {
  Point position;
  float angle;
  bool alive;
  SDL_Color color;
  Point *path;
  int path_length;
  int path_capacity;
  int bullets_left;
  Bullet bullets[MAX_BULLETS];
  float speed_boost_time_left;
  bool speed_boost_active;
  bool is_ghost;
  InputState input;
  time_t invincibility_end;
  float last_shot_time;
  pthread_mutex_t input_mutex;
} Worm;

typedef struct {
  char ip[INET_ADDRSTRLEN];
  int port;
} ServerInfo;

ServerInfo discovered_servers[MAX_SERVERS];
int num_discovered_servers = 0;

Worm worms[MAX_CLIENTS];
int num_worms = 0;
int sock = 0;
bool game_started = false;
int player_id = -1;
Uint32 game_start_time = 0;
Powerup powerups[MAX_POWERUPS];
int num_powerups = 0;

SDL_Texture *canvas_texture = NULL;

InputState current_input = {false, false, false};
pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;

SDL_Color colors[MAX_CLIENTS] = {
    {239, 71, 111, 255}, {247, 140, 107, 255}, {255, 209, 102, 255},
    {6, 214, 160, 255},  {17, 138, 178, 255},  {83, 141, 34, 255},
};

bool is_server = false;
int server_socket = -1;

void initWorm(Worm *worm, float startX, float startY, float angle) {
  worm->position.x = startX;
  worm->position.y = startY;
  worm->angle = angle;
  worm->alive = true;
  pthread_mutex_init(&worm->input_mutex, NULL);
  worm->path_capacity = 100; // Start with space for 100 points
  worm->path = malloc(worm->path_capacity * sizeof(Point));
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

void drawCircle(SDL_Renderer *renderer, int x, int y, int radius) {
  for (int w = 0; w < radius * 2; w++) {
    for (int h = 0; h < radius * 2; h++) {
      int dx = radius - w;
      int dy = radius - h;
      if ((dx * dx + dy * dy) <= (radius * radius)) {
        SDL_RenderDrawPoint(renderer, x + dx, y + dy);
      }
    }
  }
}

void drawThickLine(SDL_Renderer *renderer, int x1, int y1, int x2, int y2) {
  int deltaX = abs(x2 - x1);
  int deltaY = abs(y2 - y1);
  if (deltaX > SCREEN_WIDTH / 2 || deltaY > SCREEN_HEIGHT / 2) {
    return;
  }

  for (int dy = -WORM_RADIUS; dy <= WORM_RADIUS; dy++) {
    for (int dx = -WORM_RADIUS; dx <= WORM_RADIUS; dx++) {
      if (dx * dx + dy * dy <= WORM_RADIUS * WORM_RADIUS) {
        SDL_RenderDrawLine(renderer, x1 + dx, y1 + dy, x2 + dx, y2 + dy);
      }
    }
  }
}

void drawWorm(SDL_Renderer *renderer, Worm *worm) {
  if (!worm->alive) {
    return;
  }

  Uint8 alpha = worm->is_ghost ? 128 : 255;
  SDL_SetRenderDrawColor(renderer, worm->color.r, worm->color.g, worm->color.b,
                         alpha);

  for (int i = 1; i < worm->path_length; i++) {
    drawThickLine(renderer, (int)(worm->path[i - 1].x + 0.5),
                  (int)(worm->path[i - 1].y + 0.5),
                  (int)(worm->path[i].x + 0.5), (int)(worm->path[i].y + 0.5));
  }

  SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255); // White bullets
  for (int i = 0; i < MAX_BULLETS; i++) {
    if (worm->bullets[i].active) {
      drawCircle(renderer, (int)worm->bullets[i].position.x,
                 (int)worm->bullets[i].position.y, BULLET_RADIUS);
    }
  }

  if (worm->speed_boost_active) {
    SDL_SetRenderDrawColor(renderer, 255, 255, 0, 255); // Yellow indicator
    drawCircle(renderer, (int)worm->position.x, (int)worm->position.y,
               WORM_RADIUS + 5);
  }
}

void drawPowerups(SDL_Renderer *renderer) {
  for (int i = 0; i < num_powerups; i++) {
    if (powerups[i].type == POWERUP_BULLETS) {
      SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255); // Red for bullets
    } else if (powerups[i].type == POWERUP_SPEED) {
      SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255); // Green for speed boost
    } else if (powerups[i].type == POWERUP_GHOST) {
      SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255); // Blue for ghost
    }
    drawCircle(renderer, (int)powerups[i].position.x,
               (int)powerups[i].position.y, POWERUP_RADIUS);
  }
}

void send_input() {
  pthread_mutex_lock(&input_mutex);
  char input_buffer[50];
  snprintf(input_buffer, sizeof(input_buffer), "INPUT %d %d %d",
           current_input.left, current_input.right, current_input.up);
  send(sock, input_buffer, strlen(input_buffer), 0);
  pthread_mutex_unlock(&input_mutex);
}

void *send_input_thread(void *arg) {
  while (1) {
    send_input();
    usleep(1000000 / CLIENT_TICK_RATE);
  }
  return NULL;
}

void handle_server_messages() {
  char buffer[16384 * 16];
  int n;

  while ((n = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
    buffer[n] = '\0';
    printf("Received message: %.100s...\n", buffer);

    if (strncmp(buffer, "GAME_STARTED", 12) == 0) {
      game_started = true;
      game_start_time = SDL_GetTicks();
      printf("Game started!\n");
    } else if (strncmp(buffer, "GAME_OVER", 9) == 0) {
      game_started = false;
      printf("Game over!\n");
    } else if (strncmp(buffer, "PLAYER_ID", 9) == 0) {
      sscanf(buffer, "PLAYER_ID %d", &player_id);
      printf("Assigned player ID: %d\n", player_id);
    } else if (strncmp(buffer, "STATE", 5) == 0) {
      int count = 0;
      char *token = strtok(buffer, " ");
      token = strtok(NULL, " ");
      if (token != NULL) {
        count = atoi(token);
      }

      printf("Parsed worm count: %d\n", count);

      if (count < 0 || count > MAX_CLIENTS) {
        fprintf(stderr, "Invalid worm count: %d\n", count);
        continue;
      }

      num_worms = count;

      token = strtok(NULL, " ");
      if (token != NULL) {
        num_powerups = atoi(token);
      }

      printf("Number of powerups: %d\n", num_powerups);

      for (int i = 0; i < num_powerups; i++) {
        token = strtok(NULL, " ");
        if (token == NULL)
          break;
        powerups[i].position.x = atof(token);

        token = strtok(NULL, " ");
        if (token == NULL)
          break;
        powerups[i].position.y = atof(token);

        token = strtok(NULL, " ");
        if (token == NULL)
          break;
        powerups[i].type = atoi(token);

        printf("Powerup %d: x=%.2f, y=%.2f, type=%d\n", i,
               powerups[i].position.x, powerups[i].position.y,
               powerups[i].type);
      }

      for (int i = 0; i < num_worms; i++) {
        token = strtok(NULL, " ");
        if (token == NULL)
          break;
        int path_length = atoi(token);

        token = strtok(NULL, " ");
        if (token == NULL)
          break;
        float x = atof(token);

        token = strtok(NULL, " ");
        if (token == NULL)
          break;
        float y = atof(token);

        token = strtok(NULL, " ");
        if (token == NULL)
          break;
        float angle = atof(token);

        token = strtok(NULL, " ");
        if (token == NULL)
          break;
        bool alive = atoi(token);

        token = strtok(NULL, " ");
        if (token == NULL)
          break;
        int bullets_left = atoi(token);

        token = strtok(NULL, " ");
        if (token == NULL)
          break;
        float speed_boost_time_left = atof(token);

        token = strtok(NULL, " ");
        if (token == NULL)
          break;
        bool speed_boost_active = atoi(token);

        token = strtok(NULL, " ");
        if (token == NULL)
          break;
        bool is_ghost = atoi(token);

        token = strtok(NULL, " ");
        if (token == NULL)
          break;
        int full_path_length = atoi(token);

        worms[i].position.x = x;
        worms[i].position.y = y;
        worms[i].angle = angle;
        worms[i].alive = alive;
        worms[i].color = colors[i % MAX_CLIENTS];
        worms[i].bullets_left = bullets_left;
        worms[i].speed_boost_time_left = speed_boost_time_left;
        worms[i].speed_boost_active = speed_boost_active;
        worms[i].is_ghost = is_ghost;

        if (full_path_length > worms[i].path_capacity) {
          worms[i].path_capacity = full_path_length;
          worms[i].path =
              realloc(worms[i].path, worms[i].path_capacity * sizeof(Point));
        }
        worms[i].path_length = full_path_length;

        for (int j = 0; j < MAX_BULLETS; j++) {
          token = strtok(NULL, " ");
          if (token == NULL)
            break;
          float bullet_x = atof(token);

          token = strtok(NULL, " ");
          if (token == NULL)
            break;
          float bullet_y = atof(token);

          token = strtok(NULL, " ");
          if (token == NULL)
            break;
          float bullet_angle = atof(token);

          worms[i].bullets[j].position.x = bullet_x;
          worms[i].bullets[j].position.y = bullet_y;
          worms[i].bullets[j].angle = bullet_angle;
          worms[i].bullets[j].active = (bullet_x != 0 || bullet_y != 0);
        }

        for (int j = 0; j < worms[i].path_length; j++) {
          token = strtok(NULL, " ");
          if (token == NULL)
            break;
          worms[i].path[j].x = atof(token);

          token = strtok(NULL, " ");
          if (token == NULL)
            break;
          worms[i].path[j].y = atof(token);
        }

        printf("Parsed worm %d: x=%.2f, y=%.2f, angle=%.2f, alive=%d, "
               "path_length=%d, bullets_left=%d, speed_boost_time_left=%.2f, "
               "speed_boost_active=%d, is_ghost=%d\n",
               i, worms[i].position.x, worms[i].position.y, worms[i].angle,
               worms[i].alive, worms[i].path_length, worms[i].bullets_left,
               worms[i].speed_boost_time_left, worms[i].speed_boost_active,
               worms[i].is_ghost);
      }
    }
  }

  if (n == 0) {
    printf("Server disconnected\n");
  } else if (n < 0) {
    perror("recv failed");
  }
}

void *handle_client(void *arg) {
  int client_socket = *(int *)arg;
  char buffer[1024];
  int n;

  while ((n = recv(client_socket, buffer, sizeof(buffer) - 1, 0)) > 0) {
    buffer[n] = '\0';
    // Process client messages (implement game logic here)
    printf("Received from client: %s\n", buffer);
  }

  close(client_socket);
  return NULL;
}

void *run_server(void *arg) {
  server_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket == -1) {
    perror("socket failed");
    return NULL;
  }

  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(SERVER_PORT);

  if (bind(server_socket, (struct sockaddr *)&address, sizeof(address)) < 0) {
    perror("bind failed");
    return NULL;
  }

  if (listen(server_socket, 3) < 0) {
    perror("listen");
    return NULL;
  }

  printf("Server is listening on port %d\n", SERVER_PORT);

  while (1) {
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);
    int new_socket =
        accept(server_socket, (struct sockaddr *)&client_addr, &addrlen);
    if (new_socket < 0) {
      perror("accept");
      continue;
    }

    pthread_t client_thread;
    int *client_sock = malloc(sizeof(int));
    *client_sock = new_socket;
    if (pthread_create(&client_thread, NULL, handle_client, client_sock) != 0) {
      perror("pthread_create");
      close(new_socket);
      free(client_sock);
    } else {
      pthread_detach(client_thread);
    }
  }

  return NULL;
}

void *handle_discovery(void *arg) {
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return NULL;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(DISCOVERY_PORT);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(sockfd);
    return NULL;
  }

  char buffer[1024];
  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  while (1) {
    int received = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                            (struct sockaddr *)&client_addr, &client_addr_len);
    if (received < 0) {
      perror("recvfrom");
      continue;
    }
    buffer[received] = '\0';

    if (strcmp(buffer, "BATTLE_NOODLES_DISCOVER") == 0) {
      char response[] = "BATTLE_NOODLES_SERVER";
      if (sendto(sockfd, response, strlen(response), 0,
                 (struct sockaddr *)&client_addr, client_addr_len) < 0) {
        perror("sendto");
      }
    }
  }

  close(sockfd);
  return NULL;
}

void *discover_servers(void *arg) {
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return NULL;
  }

  int broadcast = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast,
                 sizeof(broadcast)) < 0) {
    perror("setsockopt (SO_BROADCAST)");
    close(sockfd);
    return NULL;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(DISCOVERY_PORT);
  addr.sin_addr.s_addr = INADDR_BROADCAST;

  char message[] = "BATTLE_NOODLES_DISCOVER";
  if (sendto(sockfd, message, strlen(message), 0, (struct sockaddr *)&addr,
             sizeof(addr)) < 0) {
    perror("sendto");
    close(sockfd);
    return NULL;
  }

  struct timeval tv;
  tv.tv_sec = 2;
  tv.tv_usec = 0;
  if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    perror("setsockopt (SO_RCVTIMEO)");
    close(sockfd);
    return NULL;
  }

  num_discovered_servers = 0;
  while (num_discovered_servers < MAX_SERVERS) {
    struct sockaddr_in server_addr;
    socklen_t server_addr_len = sizeof(server_addr);
    int received = recvfrom(sockfd, message, sizeof(message) - 1, 0,
                            (struct sockaddr *)&server_addr, &server_addr_len);
    if (received < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("recvfrom");
      }
      break;
    }
    message[received] = '\0';

    if (strcmp(message, "BATTLE_NOODLES_SERVER") == 0) {
      inet_ntop(AF_INET, &(server_addr.sin_addr),
                discovered_servers[num_discovered_servers].ip, INET_ADDRSTRLEN);
      discovered_servers[num_discovered_servers].port =
          ntohs(server_addr.sin_port);
      num_discovered_servers++;
    }
  }

  close(sockfd);
  return NULL;
}

void show_server_selection() {
  printf("Discovered servers:\n");
  for (int i = 0; i < num_discovered_servers; i++) {
    printf("%d. %s:%d\n", i + 1, discovered_servers[i].ip,
           discovered_servers[i].port);
  }
  printf("Enter the number of the server you want to join (0 to refresh): ");
}

int main(int argc, char *args[]) {
  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;
  SDL_Event event;
  bool quit = false;

  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    printf("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
    return 1;
  }

  window = SDL_CreateWindow("Battle Noodles", SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH,
                            SCREEN_HEIGHT, SDL_WINDOW_SHOWN);
  if (window == NULL) {
    printf("Window could not be created! SDL_Error: %s\n", SDL_GetError());
    return 1;
  }

  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  if (renderer == NULL) {
    printf("Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
    return 1;
  }

  canvas_texture =
      SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                        SDL_TEXTUREACCESS_TARGET, SCREEN_WIDTH, SCREEN_HEIGHT);
  if (canvas_texture == NULL) {
    printf("Canvas texture could not be created! SDL_Error: %s\n",
           SDL_GetError());
    return 1;
  }

  SDL_SetRenderTarget(renderer, canvas_texture);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  SDL_RenderClear(renderer);
  SDL_SetRenderTarget(renderer, NULL);

  printf("1. Host a game\n2. Join a game\nEnter your choice: ");
  int choice;
  scanf("%d", &choice);

  if (choice == 1) {
    is_server = true;
    pthread_t server_thread, discovery_thread;
    pthread_create(&server_thread, NULL, run_server, NULL);
    pthread_create(&discovery_thread, NULL, handle_discovery, NULL);

    // Initialize game as both server and client
    sock = server_socket;
    player_id = 0;
    game_started = true;
  } else if (choice == 2) {
    pthread_t discover_thread;
    pthread_create(&discover_thread, NULL, discover_servers, NULL);
    pthread_join(discover_thread, NULL);

    show_server_selection();
    int server_choice;
    scanf("%d", &server_choice);

    if (server_choice > 0 && server_choice <= num_discovered_servers) {
      struct sockaddr_in serv_addr;

      if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("\n Socket creation error \n");
        return -1;
      }

      serv_addr.sin_family = AF_INET;
      serv_addr.sin_port = htons(discovered_servers[server_choice - 1].port);

      if (inet_pton(AF_INET, discovered_servers[server_choice - 1].ip,
                    &serv_addr.sin_addr) <= 0) {
        printf("\nInvalid address/ Address not supported \n");
        return -1;
      }

      if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("\nConnection Failed \n");
        return -1;
      }

      printf("Connected to server successfully\n");
      send(sock, "JOIN", 4, 0);
    }
  }

  // Initialize worms
  for (int i = 0; i < MAX_CLIENTS; i++) {
    worms[i].path_capacity = 100; // Start with space for 100 points
    worms[i].path = malloc(worms[i].path_capacity * sizeof(Point));
    worms[i].path_length = 0;
  }

  pthread_t server_thread, input_thread;
  pthread_create(&server_thread, NULL,
                 (void *(*)(void *))handle_server_messages, NULL);
  pthread_create(&input_thread, NULL, send_input_thread, NULL);

  Uint32 lastTime = SDL_GetTicks();
  const Uint8 *keystate = SDL_GetKeyboardState(NULL);

  while (!quit) {
    Uint32 currentTime = SDL_GetTicks();
    float deltaTime = (currentTime - lastTime) / 1000.0f;
    lastTime = currentTime;

    while (SDL_PollEvent(&event) != 0) {
      if (event.type == SDL_QUIT) {
        quit = true;
      } else if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
        switch (event.key.keysym.sym) {
        case SDLK_SPACE:
          if (!game_started) {
            send(sock, "START", 5, 0);
          }
          break;
        }
      }
    }

    // Continuous input handling
    const Uint8 *keystate = SDL_GetKeyboardState(NULL);
    pthread_mutex_lock(&input_mutex);
    current_input.left = keystate[SDL_SCANCODE_LEFT];
    current_input.right = keystate[SDL_SCANCODE_RIGHT];
    current_input.up = keystate[SDL_SCANCODE_UP];
    pthread_mutex_unlock(&input_mutex);

    SDL_SetRenderDrawColor(renderer, 50, 50, 50, 255); // Dark gray background
    SDL_RenderClear(renderer);

    drawPowerups(renderer);

    for (int i = 0; i < num_worms; i++) {
      drawWorm(renderer, &worms[i]);
    }

    SDL_RenderPresent(renderer);

    SDL_Delay(1000 / CLIENT_TICK_RATE);
  }

  // Cleanup
  for (int i = 0; i < MAX_CLIENTS; i++) {
    free(worms[i].path);
  }

  close(sock);
  if (is_server && server_socket != -1) {
    close(server_socket);
  }
  SDL_DestroyTexture(canvas_texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
