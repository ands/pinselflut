#define _DEFAULT_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <SDL.h>
#include <signal.h>

#define WIDTH 1280
#define HEIGHT 720

static inline int itoa(int n, char *s) // positive integers only!
{
	int i = 0;
	do { s[i++] = n % 10 + '0'; } while ((n /= 10) > 0);
	for (int k = 0, j = i - 1; k < j; k++, j--)
	{
		char c = s[k];
		s[k] = s[j];
		s[j] = c;
	}
	return i;
}

static void error(const char *msg)
{
	perror(msg);
	exit(0);
}

static char *hostname;
static int port;
static int sockfd = 0;
static void flutConnect()
{
	if (sockfd)
		close(sockfd);

	struct hostent *server;
	server = gethostbyname(hostname);
	if (server == NULL)
		error("ERROR no such host\n");
	struct sockaddr_in serv_addr;
	bzero((char *) &serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	bcopy(server->h_addr_list[0], (char *)&serv_addr.sin_addr.s_addr, server->h_length);
	serv_addr.sin_port = htons(port);

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) 
		error("ERROR opening socket\n");

	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &(int){ 1 }, sizeof(int)) < 0)
		error("setsockopt(SO_REUSEPORT) failed\n");
	
	if (connect(sockfd,(struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
		error("ERROR connecting\n");

	fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);
	signal(SIGPIPE, SIG_IGN);
}

SDL_Renderer *renderer;
SDL_Texture *texture;

static unsigned char buffer[1024], *p = buffer;
static uint8_t *pixels;
static void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT)
		return;

	// flush buffer if necessary
	while (buffer + 1024 - p < 32)
	{
		int n;
		do
		{
			n = write(sockfd, buffer, p - buffer);
		} while(n < 0 && errno == EAGAIN);
		if (n < 0)
		{
			if (errno == EPIPE)
			{
				printf("reconnecting.\n");
				flutConnect();
			}
			else
			{
				printf("ERROR %d writing to socket\n", errno);
				exit(1);
			}
		}
		if (n > 0)
		{
			memmove(buffer, buffer + n, p - (buffer + n));
			p -= n;
		}
	}

	// add pixel to buffer
	*p++ = 'P'; *p++ = 'X'; *p++ = ' ';
	p += itoa(x, p); *p++ = ' ';
	p += itoa(y, p); *p++ = ' ';
	const unsigned char hex[] = "0123456789abcdef";
	*p++ = hex[r >> 4]; *p++ = hex[r & 0xf];
	*p++ = hex[g >> 4]; *p++ = hex[g & 0xf];
	*p++ = hex[b >> 4]; *p++ = hex[b & 0xf];
	*p++ = hex[a >> 4]; *p++ = hex[a & 0xf];
	*p++ = '\n';
	
	// set pixel locally
	pixels[(y * WIDTH + x) * 4 + 0] = (pixels[(y * WIDTH + x) * 4 + 0] * (255 - a) + r * a) >> 8;
	pixels[(y * WIDTH + x) * 4 + 1] = (pixels[(y * WIDTH + x) * 4 + 1] * (255 - a) + g * a) >> 8;
	pixels[(y * WIDTH + x) * 4 + 2] = (pixels[(y * WIDTH + x) * 4 + 2] * (255 - a) + b * a) >> 8;
}

static void fillPoint(int x, int y, int size, uint8_t r, uint8_t g, uint8_t b)
{
	float radius = size / 2.0f;
	x -= (int)ceilf(radius);
	y -= (int)ceilf(radius);
	for (int yi = 0; yi < size; yi++)
	{
		float dy = abs(yi - radius);
		for (int xi = 0; xi < size; xi++)
		{
			float dx = abs(xi - radius);
			float a = 1.0f - (sqrtf(dx * dx + dy * dy) / radius);
			if (a < 0.0f) a = 0.0f;
			a = powf(a, 7.0f);
			if (a > 0.03f)
				setPixel(x + xi, y + yi, r, g, b, (uint8_t)(a * 255.0));
		}
	}
}

static void fillRect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	for (int yi = 0; yi < h; yi++)
	{
		for (int xi = 0; xi < w; xi++)
			setPixel(x + xi, y + yi, r, g, b, a);
		if (!(yi & 1))
		{
			SDL_UpdateTexture(texture, NULL, pixels, WIDTH * 4);
			SDL_RenderCopy(renderer, texture, NULL, NULL);
			SDL_RenderPresent(renderer);
		}
	}
}

static void drawLine(int x0, int y0, int x1, int y1, int radius, uint8_t r, uint8_t g, uint8_t b)
{
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1; 
	int err = (dx > dy ? dx : -dy) / 2, e2;

	for(;;)
	{
		fillPoint(x0, y0, radius, r, g, b); // TODO: optimize number of sent pixels!
		if (x0 == x1 && y0 == y1)
			break;
		e2 = err;
		if (e2 > -dx) { err -= dy; x0 += sx; }
		if (e2 <  dy) { err += dx; y0 += sy; }
	}
}

int main(int argc, char *argv[])
{
	if (argc < 3)
	{
		fprintf(stderr,"usage %s hostname port\n", argv[0]);
		exit(0);
	}
	hostname = argv[1];
	port = atoi(argv[2]);
	flutConnect();
	
	// TODO: retrieve server resolution with SIZE command!
	
	SDL_Init(SDL_INIT_VIDEO);
	SDL_Window* window = SDL_CreateWindow("pinselflut", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIDTH, HEIGHT, SDL_WINDOW_SHOWN);
	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	SDL_RenderClear(renderer);
	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, WIDTH, HEIGHT);
	if(!texture)
	{
		printf("could not create texture\n");
		SDL_Quit();
		return 1;
	}
	pixels = calloc(WIDTH * HEIGHT * 4, 1);
	
	SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
	SDL_DisplayMode mode;
	SDL_GetCurrentDisplayMode(0, &mode);
	
	int x = 0, y = 0, draw = 0, erase = 0;
	uint8_t r = 255, g = 255, b = 255;
	int size = 7;
	int idleCounter = 0;
	while(42)
	{
		SDL_Event event;
		if(SDL_PollEvent(&event))
		{
			if (event.type == SDL_QUIT)
				break;
			if (event.type == SDL_KEYDOWN)
			{
				if (event.key.keysym.sym == SDLK_ESCAPE) // quit
					break;
				
				if (event.key.keysym.sym == SDLK_SPACE) // clear
					fillRect(0, 0, WIDTH, HEIGHT, 0, 0, 0, 255);
				
				if (event.key.keysym.sym == SDLK_r) // step red component
					r += 16;
				if (event.key.keysym.sym == SDLK_g) // step green component
					g += 16;
				if (event.key.keysym.sym == SDLK_b) // step blue component
					b += 16;
				
				if (event.key.keysym.sym == SDLK_PLUS) // increase brush size
					size++;
				if (event.key.keysym.sym == SDLK_MINUS) // decrease brush size
					size--;
				if (size < 1) size = 1;
				if (size > 32) size = 32;
			}
			if (event.type == SDL_MOUSEBUTTONDOWN)
			{
				if (event.button.button == SDL_BUTTON_LEFT) // draw
					draw = 1;
				if (event.button.button == SDL_BUTTON_MIDDLE) // erase
					erase = 1;
			}
			if (event.type == SDL_MOUSEBUTTONUP)
			{
				if (event.button.button == SDL_BUTTON_LEFT)
					draw = 0;
				if (event.button.button == SDL_BUTTON_MIDDLE)
					erase = 0;
			}
			if (event.type == SDL_MOUSEMOTION)
			{
				int nx = WIDTH * event.motion.x / mode.w;
				int ny = HEIGHT * event.motion.y / mode.h;

				if (erase)
					drawLine(x, y, nx, ny, size * 2, 0, 0, 0);
				else if (draw)
					drawLine(x, y, nx, ny, size, r, g, b);
				x = nx; y = ny;
			}
		}
		else
		{
			if (++idleCounter >= 60)
			{
				idleCounter = 0;
				const char nl = '\n';
				int n = write(sockfd, &nl, 1); // keep alive
			}
			SDL_UpdateTexture(texture, NULL, pixels, WIDTH * 4);
			SDL_RenderCopy(renderer, texture, NULL, NULL);
			SDL_RenderPresent(renderer);
		}
	}
	free(pixels);
	SDL_DestroyWindow(window);
	SDL_Quit();

	close(sockfd);
	return 0;
}
