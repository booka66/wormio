#include "/opt/homebrew/Cellar/sdl2/2.30.6/include/SDL2/SDL.h"
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

#define SCREEN_WIDTH 1024
#define SCREEN_HEIGHT 640
#define WORM_RADIUS 3
#define MAX_WORMS 20
#define INPUT_BUFFER_SIZE 10
#define WORM_SPEED 2.0
#define TURN_SPEED 0.1
#define CLIENT_TICK_RATE 60 // Hz
#define GRACE_PERIOD 3000   // 3 seconds in milliseconds
#define MAX_PATH_LENGTH 200

typedef struct {
  float x;
  float y;
} Point;

typedef struct {
  Point position;
  float angle;
  bool alive;
  SDL_Color color;
  Point path[MAX_PATH_LENGTH];
  int path_length;
} Worm;

Worm worms[MAX_WORMS];
int num_worms = 0;
int sock = 0;
bool game_started = false;
int player_id = -1;
Uint32 game_start_time = 0;

SDL_Texture *canvas_texture = NULL;

char input_buffer[INPUT_BUFFER_SIZE][10];
int input_buffer_count = 0;
pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;

SDL_Color colors[MAX_WORMS] = {
    {255, 0, 0, 255},     // Red
    {0, 255, 0, 255},     // Green
    {0, 0, 255, 255},     // Blue
    {255, 255, 0, 255},   // Yellow
    {255, 0, 255, 255},   // Magenta
    {0, 255, 255, 255},   // Cyan
    {255, 128, 0, 255},   // Orange
    {128, 0, 255, 255},   // Purple
    {0, 255, 128, 255},   // Lime
    {128, 255, 0, 255},   // Chartreuse
    {255, 0, 128, 255},   // Pink
    {0, 128, 255, 255},   // Sky Blue
    {255, 128, 128, 255}, // Light Red
    {128, 255, 128, 255}, // Light Green
    {128, 128, 255, 255}, // Light Blue
    {255, 255, 128, 255}, // Light Yellow
    {255, 128, 255, 255}, // Light Magenta
    {128, 255, 255, 255}, // Light Cyan
    {192, 192, 192, 255}, // Light Gray
    {128, 128, 128, 255}  // Gray
};

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
  SDL_SetRenderDrawColor(renderer, worm->color.r, worm->color.g, worm->color.b,
                         worm->alive ? 255 : 64);

  for (int i = 1; i < worm->path_length; i++) {
    drawThickLine(renderer, (int)(worm->path[i - 1].x + 0.5),
                  (int)(worm->path[i - 1].y + 0.5),
                  (int)(worm->path[i].x + 0.5), (int)(worm->path[i].y + 0.5));
  }
}

bool checkCollision(SDL_Renderer *renderer, int x, int y) {
  SDL_Rect pixel_rect = {x - WORM_RADIUS, y - WORM_RADIUS, WORM_RADIUS * 2 + 1,
                         WORM_RADIUS * 2 + 1};

  // Create a surface to read pixels into
  SDL_Surface *surface =
      SDL_CreateRGBSurface(0, pixel_rect.w, pixel_rect.h, 32, 0, 0, 0, 0);
  if (surface == NULL) {
    printf("Failed to create surface for collision detection: %s\n",
           SDL_GetError());
    return false;
  }

  // Read pixels from the canvas texture
  SDL_SetRenderTarget(renderer, canvas_texture);
  SDL_RenderReadPixels(renderer, &pixel_rect, SDL_PIXELFORMAT_RGBA8888,
                       surface->pixels, surface->pitch);
  SDL_SetRenderTarget(renderer, NULL);

  // Check pixels
  Uint32 *pixels = (Uint32 *)surface->pixels;
  int total_pixels = pixel_rect.w * pixel_rect.h;

  for (int i = 0; i < total_pixels; i++) {
    Uint8 r, g, b, a;
    SDL_GetRGBA(pixels[i], surface->format, &r, &g, &b, &a);

    // Check if the pixel is not black (0, 0, 0)
    if (r != 0 || g != 0 || b != 0) {
      SDL_FreeSurface(surface);
      return true; // Collision detected
    }
  }

  SDL_FreeSurface(surface);
  return false; // No collision
}

void updateLocalWorm(SDL_Renderer *renderer, Worm *worm, const char *input,
                     float deltaTime) {
  if (!worm->alive || SDL_GetTicks() - game_start_time < GRACE_PERIOD)
    return;

  // Update angle based on input
  if (strcmp(input, "LEFT") == 0) {
    worm->angle -= TURN_SPEED * deltaTime;
  } else if (strcmp(input, "RIGHT") == 0) {
    worm->angle += TURN_SPEED * deltaTime;
  }

  // Calculate new position
  float newX = worm->position.x + cos(worm->angle) * WORM_SPEED * deltaTime;
  float newY = worm->position.y + sin(worm->angle) * WORM_SPEED * deltaTime;

  // Wrap around screen edges
  newX = fmod(newX + SCREEN_WIDTH, SCREEN_WIDTH);
  newY = fmod(newY + SCREEN_HEIGHT, SCREEN_HEIGHT);

  // Check for collision at the new head position
  if (checkCollision(renderer, (int)newX, (int)newY)) {
    worm->alive = false;
    printf("COLLISION DETECTED! WORM DIED AT (%d, %d)\n", (int)newX, (int)newY);
    return;
  }

  // Update position and draw
  Point lastPosition = worm->position;
  worm->position.x = newX;
  worm->position.y = newY;

  SDL_SetRenderTarget(renderer, canvas_texture);
  SDL_SetRenderDrawColor(renderer, worm->color.r, worm->color.g, worm->color.b,
                         255);
  drawThickLine(renderer, (int)lastPosition.x, (int)lastPosition.y, (int)newX,
                (int)newY);
  SDL_SetRenderTarget(renderer, NULL);
}

void renderWorms(SDL_Renderer *renderer) {
  for (int i = 0; i < num_worms; i++) {
    Worm *worm = &worms[i];
    if (!worm->alive) {
      continue;
    }

    Point lastPosition = {worm->position.x - cos(worm->angle) * WORM_SPEED,
                          worm->position.y - sin(worm->angle) * WORM_SPEED};

    SDL_SetRenderTarget(renderer, canvas_texture);
    drawWorm(renderer, worm);
    SDL_SetRenderTarget(renderer, NULL);
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

        worms[i].position.x = x;
        worms[i].position.y = y;
        worms[i].angle = angle;
        worms[i].alive = alive;
        worms[i].color = colors[i % MAX_WORMS];

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
               "path_length=%d\n",
               i, worms[i].position.x, worms[i].position.y, worms[i].angle,
               worms[i].alive, worms[i].path_length);
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
            game_start_time = SDL_GetTicks();
          }
          break;
        }
      }
    }

    const char *input = NULL;
    if (keystate[SDL_SCANCODE_LEFT]) {
      input = "LEFT";
    } else if (keystate[SDL_SCANCODE_RIGHT]) {
      input = "RIGHT";
    }

    if (input) {
      send_input(input);
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    for (int i = 0; i < num_worms; i++) {
      drawWorm(renderer, &worms[i]);
    }

    SDL_RenderPresent(renderer);

    SDL_Delay(1000 / CLIENT_TICK_RATE);
  }

  // Cleanup
  for (int i = 0; i < num_worms; i++) {
    free(worms[i].path);
  }

  close(sock);
  SDL_DestroyTexture(canvas_texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
