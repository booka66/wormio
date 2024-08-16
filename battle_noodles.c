#include "/opt/homebrew/Cellar/sdl2/2.30.6/include/SDL2/SDL.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 600
#define INITIAL_WORM_LENGTH 10
#define WORM_SPEED 1.2
#define TURN_SPEED 0.07
#define PI 3.14159265
#define WORM_RADIUS 5
#define COLLISION_DISTANCE (WORM_RADIUS * 1.5)
#define INITIAL_LENGTH 1
#define COLLISION_START_LENGTH 20
#define GROWTH_RATE 1

// Define pastel orange color
#define PASTEL_ORANGE_R 255
#define PASTEL_ORANGE_G 179
#define PASTEL_ORANGE_B 71

// Define pastel blue color
#define PASTEL_BLUE_R 135
#define PASTEL_BLUE_G 206
#define PASTEL_BLUE_B 250

typedef struct {
  float x;
  float y;
} Point;

typedef struct {
  Point *positions;
  int capacity;
  float angle;
  SDL_Texture *trailTexture;
  bool alive;
  int currentLength;
  SDL_Color color;
} Worm;

void drawCircle(SDL_Renderer *renderer, int centerX, int centerY, int radius) {
  for (int y = -radius; y <= radius; y++) {
    for (int x = -radius; x <= radius; x++) {
      if (x * x + y * y <= radius * radius) {
        SDL_RenderDrawPoint(renderer, centerX + x, centerY + y);
      }
    }
  }
}

void initWorm(Worm *worm, SDL_Renderer *renderer, SDL_Color color, float startX,
              float startY) {
  worm->capacity = INITIAL_WORM_LENGTH;
  worm->positions = (Point *)malloc(worm->capacity * sizeof(Point));
  worm->angle = rand() % 360 * PI / 180;
  worm->alive = true;
  worm->currentLength = INITIAL_LENGTH;
  worm->color = color;

  // Initialize the first segment
  worm->positions[0].x = startX;
  worm->positions[0].y = startY;

  // Initialize the rest of the segments behind the first one
  for (int i = 1; i < worm->capacity; i++) {
    worm->positions[i].x = worm->positions[0].x - (i * cos(worm->angle));
    worm->positions[i].y = worm->positions[0].y - (i * sin(worm->angle));
  }

  // Create a texture for the trail
  worm->trailTexture =
      SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                        SDL_TEXTUREACCESS_TARGET, SCREEN_WIDTH, SCREEN_HEIGHT);
  SDL_SetTextureBlendMode(worm->trailTexture, SDL_BLENDMODE_BLEND);
}

bool checkCollision(Worm *worm1, Worm *worm2) {
  if (worm1->currentLength < COLLISION_START_LENGTH)
    return false;

  Point head = worm1->positions[0];

  // Check collision with self
  for (int i = COLLISION_START_LENGTH / 2; i < worm1->currentLength; i++) {
    Point segment = worm1->positions[i];
    float dx = head.x - segment.x;
    float dy = head.y - segment.y;
    float distanceSquared = dx * dx + dy * dy;

    if (distanceSquared < COLLISION_DISTANCE * COLLISION_DISTANCE) {
      return true;
    }
  }

  // Check collision with other worm
  for (int i = 0; i < worm2->currentLength; i++) {
    Point segment = worm2->positions[i];
    float dx = head.x - segment.x;
    float dy = head.y - segment.y;
    float distanceSquared = dx * dx + dy * dy;

    if (distanceSquared < COLLISION_DISTANCE * COLLISION_DISTANCE) {
      return true;
    }
  }

  return false;
}

void growWorm(Worm *worm) {
  if (worm->currentLength >= worm->capacity) {
    int newCapacity = worm->capacity * 2;
    Point *newPositions =
        (Point *)realloc(worm->positions, newCapacity * sizeof(Point));

    if (newPositions == NULL) {
      // Handle memory allocation failure
      printf("Failed to allocate memory for worm growth.\n");
      return;
    }

    worm->positions = newPositions;
    worm->capacity = newCapacity;
  }

  worm->currentLength += GROWTH_RATE;
}

void updateWorm(Worm *worm) {
  if (!worm->alive)
    return;

  float newX = worm->positions[0].x + cos(worm->angle) * WORM_SPEED;
  float newY = worm->positions[0].y + sin(worm->angle) * WORM_SPEED;

  for (int i = worm->currentLength - 1; i > 0; i--) {
    worm->positions[i] = worm->positions[i - 1];
  }

  worm->positions[0].x = newX;
  worm->positions[0].y = newY;

  // Wrap around screen edges
  if (worm->positions[0].x < 0)
    worm->positions[0].x = SCREEN_WIDTH;
  if (worm->positions[0].x > SCREEN_WIDTH)
    worm->positions[0].x = 0;
  if (worm->positions[0].y < 0)
    worm->positions[0].y = SCREEN_HEIGHT;
  if (worm->positions[0].y > SCREEN_HEIGHT)
    worm->positions[0].y = 0;

  growWorm(worm);
}

void drawWorm(SDL_Renderer *renderer, Worm *worm) {
  SDL_SetRenderTarget(renderer, worm->trailTexture);

  SDL_RenderCopy(renderer, worm->trailTexture, NULL, NULL);

  SDL_SetRenderDrawColor(renderer, worm->color.r, worm->color.g, worm->color.b,
                         worm->alive ? 255 : 128);
  drawCircle(renderer, (int)worm->positions[0].x, (int)worm->positions[0].y,
             WORM_RADIUS);

  SDL_SetRenderTarget(renderer, NULL);

  SDL_RenderCopy(renderer, worm->trailTexture, NULL, NULL);
}

void cleanupWorm(Worm *worm) {
  free(worm->positions);
  SDL_DestroyTexture(worm->trailTexture);
}

int main(int argc, char *args[]) {
  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;
  SDL_Event event;
  bool quit = false;
  Worm worm1, worm2;

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

  SDL_Color orange = {PASTEL_ORANGE_R, PASTEL_ORANGE_G, PASTEL_ORANGE_B, 255};
  SDL_Color blue = {PASTEL_BLUE_R, PASTEL_BLUE_G, PASTEL_BLUE_B, 255};

  initWorm(&worm1, renderer, orange, SCREEN_WIDTH / 3.0, SCREEN_HEIGHT / 2.0);
  initWorm(&worm2, renderer, blue, 2 * SCREEN_WIDTH / 3.0, SCREEN_HEIGHT / 2.0);

  while (!quit) {
    while (SDL_PollEvent(&event) != 0) {
      if (event.type == SDL_QUIT) {
        quit = true;
      }
    }

    const Uint8 *currentKeyStates = SDL_GetKeyboardState(NULL);
    if (currentKeyStates[SDL_SCANCODE_LEFT]) {
      worm1.angle -= TURN_SPEED;
    }
    if (currentKeyStates[SDL_SCANCODE_RIGHT]) {
      worm1.angle += TURN_SPEED;
    }
    if (currentKeyStates[SDL_SCANCODE_A]) {
      worm2.angle -= TURN_SPEED;
    }
    if (currentKeyStates[SDL_SCANCODE_D]) {
      worm2.angle += TURN_SPEED;
    }

    updateWorm(&worm1);
    updateWorm(&worm2);

    if (checkCollision(&worm1, &worm2)) {
      worm1.alive = false;
    }
    if (checkCollision(&worm2, &worm1)) {
      worm2.alive = false;
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    drawWorm(renderer, &worm1);
    drawWorm(renderer, &worm2);

    SDL_RenderPresent(renderer);
    SDL_Delay(8); // Cap at roughly 120 FPS

    if (!worm1.alive || !worm2.alive) {
      SDL_Delay(2000); // Wait for 2 seconds before quitting
      quit = true;
    }
  }

  cleanupWorm(&worm1);
  cleanupWorm(&worm2);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
