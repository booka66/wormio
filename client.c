#include <SDL.h>
#include <SDL_ttf.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_WORMS 6
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
#define CLIENT_TICK_RATE 60 // Hz

#define MAX_SERVERS 10
#define DISCOVERY_PORT 8081
#define SERVER_RESPONSE_TIMEOUT 2

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
} Worm;

Worm worms[MAX_WORMS];
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

SDL_Color colors[MAX_WORMS] = {
    {239, 71, 111, 255}, {247, 140, 107, 255}, {255, 209, 102, 255},
    {6, 214, 160, 255},  {17, 138, 178, 255},  {83, 141, 34, 255},
};

typedef struct {
  char ip[INET_ADDRSTRLEN];
  int port;
  char name[64];
} ServerInfo;

ServerInfo servers[MAX_SERVERS];
int num_servers = 0;

TTF_Font *font = NULL;

void render_text(SDL_Renderer *renderer, const char *text, int x, int y,
                 SDL_Color color) {
  SDL_Surface *surface = TTF_RenderText_Solid(font, text, color);
  if (surface == NULL) {
    printf("Unable to render text surface! SDL_ttf Error: %s\n",
           TTF_GetError());
    return;
  }

  SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
  if (texture == NULL) {
    printf("Unable to create texture from rendered text! SDL Error: %s\n",
           SDL_GetError());
    SDL_FreeSurface(surface);
    return;
  }

  SDL_Rect dest = {x, y, surface->w, surface->h};
  SDL_RenderCopy(renderer, texture, NULL, &dest);

  SDL_FreeSurface(surface);
  SDL_DestroyTexture(texture);
}

void discover_servers() {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    perror("Socket creation failed");
    return;
  }

  int broadcast = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast,
                 sizeof(broadcast)) < 0) {
    perror("Set socket option failed");
    close(sock);
    return;
  }

  struct sockaddr_in broadcast_addr;
  memset(&broadcast_addr, 0, sizeof(broadcast_addr));
  broadcast_addr.sin_family = AF_INET;
  broadcast_addr.sin_port = htons(DISCOVERY_PORT);
  broadcast_addr.sin_addr.s_addr = INADDR_BROADCAST;

  char discovery_message[] = "DISCOVER_BATTLE_NOODLES_SERVER";
  if (sendto(sock, discovery_message, strlen(discovery_message), 0,
             (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr)) < 0) {
    perror("Broadcast failed");
    close(sock);
    return;
  }

  struct timeval tv;
  tv.tv_sec = SERVER_RESPONSE_TIMEOUT;
  tv.tv_usec = 0;
  if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    perror("Set socket timeout failed");
    close(sock);
    return;
  }

  char buffer[256];
  struct sockaddr_in server_addr;
  socklen_t addr_len = sizeof(server_addr);

  while (num_servers < MAX_SERVERS) {
    int received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                            (struct sockaddr *)&server_addr, &addr_len);
    if (received < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        // Timeout reached, no more servers responding
        break;
      }
      perror("Receive failed");
      continue;
    }

    buffer[received] = '\0';
    char server_name[64];
    int server_port;
    if (sscanf(buffer, "BATTLE_NOODLES_SERVER %63s %d", server_name,
               &server_port) == 2) {
      strcpy(servers[num_servers].ip, inet_ntoa(server_addr.sin_addr));
      servers[num_servers].port = server_port;
      strcpy(servers[num_servers].name, server_name);
      num_servers++;
    }
  }

  close(sock);
}

int choose_server(SDL_Renderer *renderer) {
  SDL_Event event;
  int selected = 0;
  SDL_Color text_color = {255, 255, 255, 255};
  SDL_Color highlight_color = {255, 255, 0, 255};

  while (1) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    render_text(renderer, "Select a server:", 50, 20, text_color);

    for (int i = 0; i < num_servers; i++) {
      SDL_Color color = (i == selected) ? highlight_color : text_color;
      char server_info[128];
      snprintf(server_info, sizeof(server_info), "%d. %s (%s:%d)", i + 1,
               servers[i].name, servers[i].ip, servers[i].port);
      render_text(renderer, server_info, 50, 50 + i * 30, color);
    }

    SDL_RenderPresent(renderer);

    if (SDL_WaitEvent(&event)) {
      if (event.type == SDL_QUIT) {
        return -1;
      } else if (event.type == SDL_KEYDOWN) {
        switch (event.key.keysym.sym) {
        case SDLK_UP:
          selected = (selected - 1 + num_servers) % num_servers;
          break;
        case SDLK_DOWN:
          selected = (selected + 1) % num_servers;
          break;
        case SDLK_RETURN:
          return selected;
        case SDLK_ESCAPE:
          return -1;
        }
      }
    }
  }
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
    usleep(1000000 / CLIENT_TICK_RATE); // Send at CLIENT_TICK_RATE Hz
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

      if (count < 0 || count > MAX_WORMS) {
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
        worms[i].color = colors[i % MAX_WORMS];
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

int main(int argc, char *args[]) {
  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;
  SDL_Event event;
  bool quit = false;

  discover_servers();

  if (num_servers == 0) {
    printf("No servers found. Exiting.\n");
    return -1;
  }

  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    printf("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
    return 1;
  }

  if (TTF_Init() == -1) {
    printf("SDL_ttf could not initialize! SDL_ttf Error: %s\n", TTF_GetError());
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

  font = TTF_OpenFont("./cmunui.ttf", 24);
  if (font == NULL) {
    printf("Failed to load font! SDL_ttf Error: %s\n", TTF_GetError());
    return 1;
  }

  int selected_server = choose_server(renderer);
  if (selected_server == -1) {
    printf("No server selected. Exiting.\n");
    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return -1;
  }

  if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    printf("\n Socket creation error \n");
    return -1;
  }

  struct sockaddr_in serv_addr;
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(servers[selected_server].port);

  if (inet_pton(AF_INET, servers[selected_server].ip, &serv_addr.sin_addr) <=
      0) {
    printf("\nInvalid address/ Address not supported \n");
    return -1;
  }

  if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    printf("\nConnection Failed \n");
    return -1;
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

  send(sock, "JOIN", 4, 0);

  // Initialize worms
  for (int i = 0; i < MAX_WORMS; i++) {
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
  for (int i = 0; i < MAX_WORMS; i++) {
    free(worms[i].path);
  }

  close(sock);
  SDL_DestroyTexture(canvas_texture);
  TTF_CloseFont(font);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  TTF_Quit();
  SDL_Quit();

  return 0;
}
