#include "/opt/homebrew/Cellar/sdl2/2.30.6/include/SDL2/SDL.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_WORMS 6
#define SCREEN_WIDTH (int)(1024 * 1.5)
#define SCREEN_HEIGHT (int)(640 * 1.5)
#define WORM_SPEED 2.0
#define TURN_SPEED 0.1
#define WORM_RADIUS 3
#define MAX_PATH_LENGTH 150
#define INPUT_BUFFER_SIZE 10
#define MAX_BULLETS 3
#define BULLET_RADIUS 5
#define MAX_POWERUPS 3
#define POWERUP_RADIUS 10
#define CLIENT_TICK_RATE 60 // Hz

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
} Powerup;

typedef struct {
  Point position;
  float angle;
  bool alive;
  SDL_Color color;
  Point path[MAX_PATH_LENGTH];
  int path_length;
  int bullets_left;
  Bullet bullets[MAX_BULLETS];
  float speed_boost_time_left;
  bool speed_boost_active;
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

char input_buffer[INPUT_BUFFER_SIZE][10];
int input_buffer_count = 0;
pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;

SDL_Color colors[MAX_WORMS] = {
    {239, 71, 111, 255}, {247, 140, 107, 255}, {255, 209, 102, 255},
    {6, 214, 160, 255},  {17, 138, 178, 255},  {83, 141, 34, 255},
};

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
  // Check for wrapping by determining if the worm moved a large distance
  int deltaX = abs(x2 - x1);
  int deltaY = abs(y2 - y1);
  if (deltaX > SCREEN_WIDTH / 2 || deltaY > SCREEN_HEIGHT / 2) {
    // Skip drawing the line if it wraps across the screen
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
  // Only draw the worm if it is alive
  if (!worm->alive) {
    return;
  }
  SDL_SetRenderDrawColor(renderer, worm->color.r, worm->color.g, worm->color.b,
                         worm->alive ? 255 : 64);

  for (int i = 1; i < worm->path_length; i++) {
    drawThickLine(renderer, (int)(worm->path[i - 1].x + 0.5),
                  (int)(worm->path[i - 1].y + 0.5),
                  (int)(worm->path[i].x + 0.5), (int)(worm->path[i].y + 0.5));
  }

  // Draw bullets
  SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255); // White bullets
  for (int i = 0; i < MAX_BULLETS; i++) {
    if (worm->bullets[i].active) {
      drawCircle(renderer, (int)worm->bullets[i].position.x,
                 (int)worm->bullets[i].position.y, BULLET_RADIUS);
    }
  }

  // Draw speed boost indicator
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
    } else {
      SDL_SetRenderDrawColor(renderer, 0, 255, 0, 255); // Green for speed boost
    }
    drawCircle(renderer, (int)powerups[i].position.x,
               (int)powerups[i].position.y, POWERUP_RADIUS);
  }
}

void send_input(const char *input) {
  pthread_mutex_lock(&input_mutex);
  if (input_buffer_count < INPUT_BUFFER_SIZE) {
    strncpy(input_buffer[input_buffer_count], input,
            sizeof(input_buffer[0]) - 1);
    input_buffer[input_buffer_count][sizeof(input_buffer[0]) - 1] = '\0';
    input_buffer_count++;
  }
  pthread_mutex_unlock(&input_mutex);
}

void *send_input_thread(void *arg) {
  while (1) {
    pthread_mutex_lock(&input_mutex);
    if (input_buffer_count > 0) {
      send(sock, input_buffer[0], strlen(input_buffer[0]), 0);
      for (int i = 1; i < input_buffer_count; i++) {
        strcpy(input_buffer[i - 1], input_buffer[i]);
      }
      input_buffer_count--;
    }
    pthread_mutex_unlock(&input_mutex);
    usleep(1000000 / CLIENT_TICK_RATE); // Send at CLIENT_TICK_RATE Hz
  }
  return NULL;
}

void handle_server_messages() {
  char buffer[16384];
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

      // Parse powerup information
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

        worms[i].position.x = x;
        worms[i].position.y = y;
        worms[i].angle = angle;
        worms[i].alive = alive;
        worms[i].color = colors[i % MAX_WORMS];
        worms[i].bullets_left = bullets_left;
        worms[i].speed_boost_time_left = speed_boost_time_left;
        worms[i].speed_boost_active = speed_boost_active;

        // Parse bullet information
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

        worms[i].path_length =
            (path_length > MAX_PATH_LENGTH) ? MAX_PATH_LENGTH : path_length;

        // Parse path points
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
               "speed_boost_active=%d\n",
               i, worms[i].position.x, worms[i].position.y, worms[i].angle,
               worms[i].alive, worms[i].path_length, worms[i].bullets_left,
               worms[i].speed_boost_time_left, worms[i].speed_boost_active);
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

  struct sockaddr_in serv_addr;

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

  if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    printf("\n Socket creation error \n");
    return -1;
  }

  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(8080);

  if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
    printf("\nInvalid address/ Address not supported \n");
    return -1;
  }

  if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    printf("\nConnection Failed \n");
    return -1;
  }

  send(sock, "JOIN", 4, 0);

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
            send_input("START");
          } else {
            send_input("SHOOT");
          }
          break;
        }
      }
    }

    // Continuous input handling
    const char *input = NULL;
    if (keystate[SDL_SCANCODE_LEFT]) {
      input = "LEFT";
    } else if (keystate[SDL_SCANCODE_RIGHT]) {
      input = "RIGHT";
    } else if (keystate[SDL_SCANCODE_UP]) {
      input = "BOOST";
    }

    if (input) {
      send_input(input);
    }

    SDL_SetRenderDrawColor(renderer, 50, 50, 50, 255); // Dark gray background
    SDL_RenderClear(renderer);

    drawPowerups(renderer);

    for (int i = 0; i < num_worms; i++) {
      drawWorm(renderer, &worms[i]);
    }

    // Draw HUD
    if (player_id >= 0 && player_id < num_worms) {
      Worm *player_worm = &worms[player_id];
      char hud_text[100];
      SDL_Color text_color = {255, 255, 255, 255};
      SDL_Surface *text_surface;
      SDL_Texture *text_texture;
      SDL_Rect text_rect;

      snprintf(hud_text, sizeof(hud_text), "Bullets: %d   Speed Boost: %.1f",
               player_worm->bullets_left, player_worm->speed_boost_time_left);

      // You'll need to set up a font for this part
      // SDL_TTF_Font *font = TTF_OpenFont("path/to/font.ttf", 24);
      // text_surface = TTF_RenderText_Solid(font, hud_text, text_color);
      // text_texture = SDL_CreateTextureFromSurface(renderer, text_surface);
      // text_rect.x = 10;
      // text_rect.y = 10;
      // text_rect.w = text_surface->w;
      // text_rect.h = text_surface->h;
      // SDL_RenderCopy(renderer, text_texture, NULL, &text_rect);
      // SDL_FreeSurface(text_surface);
      // SDL_DestroyTexture(text_texture);
    }

    SDL_RenderPresent(renderer);

    SDL_Delay(1000 / CLIENT_TICK_RATE);
  }

  // Cleanup
  close(sock);
  SDL_DestroyTexture(canvas_texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
