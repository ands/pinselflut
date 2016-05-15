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
#include <signal.h>
#include <math.h>
#include "glad/glad.h"
#include <GLFW/glfw3.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_GLFW_GL3_IMPLEMENTATION
#include "nuklear.h"
#include "nuklear_glfw_gl3.h"

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
	{
		perror("ERROR no such host\n");
		exit(1);
	}
	struct sockaddr_in serv_addr;
	bzero((char *) &serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	bcopy(server->h_addr_list[0], (char *)&serv_addr.sin_addr.s_addr, server->h_length);
	serv_addr.sin_port = htons(port);

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
	{
		perror("ERROR opening socket\n");
		exit(2);
	}

	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &(int){ 1 }, sizeof(int)) < 0)
	{
		perror("setsockopt(SO_REUSEPORT) failed\n");
		exit(3);
	}
	
	if (connect(sockfd,(struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
	{
		perror("ERROR connecting\n");
		exit(4);
	}

	fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);
	signal(SIGPIPE, SIG_IGN);
}

static int pixelsWidth = 640, pixelsHeight = 480;
static uint8_t *pixels;
static void readSize()
{
	// retrieve server screen resolution using the SIZE command
	fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) & (~O_NONBLOCK)); // temporarily disable non-blocking mode
	int n = write(sockfd, "SIZE\n", 5);
	if (n == 5)
	{
		char result[256];
		n = read(sockfd, result, 256);
		if (n > 5 && !strncmp(result, "SIZE ", 5))
		{
			int w, h;
			n = sscanf(result, "SIZE %d %d", &w, &h);
			if (n == 2)
			{
				pixelsWidth = w;
				pixelsHeight = h;
				printf("Received screen size from server: %dx%d\n", pixelsWidth, pixelsHeight);
			}
			else
				printf("Bad SIZE payload!\n");
		}
		else
			printf("Bad SIZE response!\n");
	}
	else
		printf("Could not send SIZE command!\n");
	fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK); // reenable non-blocking mode
}

static unsigned char buffer[1024], *p = buffer;
static void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	if (x < 0 || y < 0 || x >= pixelsWidth || y >= pixelsHeight)
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
				fprintf(stderr, "ERROR %d writing to socket\n", errno);
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
	float alpha = a / 255.0f, nalpha = 1.0f - alpha;
	uint8_t *pixel = pixels + (y * pixelsWidth + x) * 3;
	pixel[0] = (uint8_t)(pixel[0] * nalpha + r * alpha);
	pixel[1] = (uint8_t)(pixel[1] * nalpha + g * alpha);
	pixel[2] = (uint8_t)(pixel[2] * nalpha + b * alpha);
}

static void fillPoint(int x, int y, int size, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	float radius = size / 2.0f;
	x -= (int)ceilf(radius);
	y -= (int)ceilf(radius);
	float a2 = powf(a / 255.0f, 1.0f / 5.0f);
	for (int yi = 0; yi < size; yi++)
	{
		float dy = abs(yi - radius);
		for (int xi = 0; xi < size; xi++)
		{
			float dx = abs(xi - radius);
			float alpha = a2 * (1.0f - (sqrtf(dx * dx + dy * dy) / radius));
			if (alpha > 0.0f)
			{
				alpha = powf(alpha, 7.0f);
				alpha *= 255.0f;
				if (alpha >= 1.0f)
					setPixel(x + xi, y + yi, r, g, b, (uint8_t)alpha);
			}
		}
	}
}

struct
{
	int x, y, w, h;
	uint8_t r, g, b, a;
	int currentLine;
} fillState = {0};
static void fillRect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	fillState.x = x; fillState.y = y; fillState.w = w; fillState.h = h;
	fillState.r = r; fillState.g = g; fillState.b = b; fillState.a = a;
	fillState.currentLine = 0;
}
static int fillUpdate()
{
	if (fillState.currentLine < fillState.h)
	{
		for (int x = 0; x < fillState.w; x++)
			setPixel(fillState.x + x, fillState.y + fillState.currentLine,
				fillState.r, fillState.g, fillState.b, fillState.a);
		fillState.currentLine++;
		return 1;
	}
	return 0;
}

static void drawLine(int x0, int y0, int x1, int y1, int radius, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1; 
	int err = (dx > dy ? dx : -dy) / 2, e2;

	for(;;)
	{
		fillPoint(x0, y0, radius, r, g, b, a); // TODO: optimize number of sent pixels!
		if (x0 == x1 && y0 == y1)
			break;
		e2 = err;
		if (e2 > -dx) { err -= dy; x0 += sx; }
		if (e2 <  dy) { err += dx; y0 += sy; }
	}
}

static void error_callback(int e, const char *d)
{
	printf("Error %d: %s\n", e, d);
}

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		fprintf(stderr, "usage %s hostname port\n", argv[0]);
		exit(0);
	}
	hostname = argv[1];
	port = atoi(argv[2]);
	flutConnect();
	readSize();

	glfwSetErrorCallback(error_callback);
	if (!glfwInit())
	{
		fprintf(stderr, "GFLW failed to init!\n");
		exit(1);
	}
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	#ifdef __APPLE__
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
	#endif
	GLFWwindow *window = glfwCreateWindow(232 + pixelsWidth, pixelsHeight + 96, "Pinselflut", NULL, NULL);
	glfwMakeContextCurrent(window);
	gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
	glfwSwapInterval(1);
	
	pixels = calloc(pixelsWidth * pixelsHeight * 3, 1);
	GLuint texture;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, pixelsWidth, pixelsHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels);

	struct nk_context *ctx = nk_glfw3_init(window, NK_GLFW3_INSTALL_CALLBACKS);
	struct nk_font_atlas *atlas;
	nk_glfw3_font_stash_begin(&atlas);
	nk_glfw3_font_stash_end();

	#define MAX_DRAW_BUFFER_SIZE 32
	int x[MAX_DRAW_BUFFER_SIZE] = {0}, y[MAX_DRAW_BUFFER_SIZE] = {0};
	nk_size drawBufferPos = 0, drawBufferSize = 8;
	int lx = -1, ly = -1;
	struct nk_color fg = nk_rgba(255, 255, 255, 255);
	struct nk_color bg = nk_rgba(0, 0, 0, 255);
	nk_size size = 7;
	int idleCounter = 0;
	while (!glfwWindowShouldClose(window))
	{
		glfwPollEvents();
		int w, h;
		glfwGetFramebufferSize(window, &w, &h);
		nk_glfw3_new_frame();
		
		if (++idleCounter >= 60)
		{
			idleCounter = 0;
			const char nl = '\n';
			int n = write(sockfd, &nl, 1); // keep alive
		}
		
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, pixelsWidth, pixelsHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels);
		struct nk_panel canvas;
		if (nk_begin(ctx, &canvas, "Canvas", nk_rect(200, 0, pixelsWidth + 32, pixelsHeight + 96),
			NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|//NK_WINDOW_SCALABLE|
			NK_WINDOW_MINIMIZABLE|NK_WINDOW_TITLE))
		{
			nk_layout_row_static(ctx, pixelsHeight, pixelsWidth, 1);
			struct nk_vec2 p = nk_widget_position(ctx);
			nk_image(ctx, nk_image_id(texture));

			if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS)
			{
				struct nk_color color = fg;
				if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS)
					color = bg;
				
				int nx = ctx->input.mouse.pos.x - p.x;
				int ny = ctx->input.mouse.pos.y - p.y;
				x[drawBufferPos] = nx; y[drawBufferPos] = ny;
				if (++drawBufferPos >= drawBufferSize)
				{
					int avgx = 0, avgy = 0;
					for (int i = 0; i < drawBufferSize; i++)
					{
						avgx += x[i];
						avgy += y[i];
					}
					avgx /= drawBufferSize;
					avgy /= drawBufferSize;
					if (avgx >= 0 && avgy >= 0 && avgx < pixelsWidth && avgy < pixelsHeight)
					{
						if (lx == -1 && ly == -1) { lx = avgx; ly = avgy; }
						if (avgx != lx || avgy != ly)
							drawLine(lx, ly, avgx, avgy, size, color.r, color.g, color.b, color.a);
						lx = avgx; ly = avgy;
					}
					
					for (int i = 0; i < drawBufferSize - 1; i++)
					{
						x[i] = x[i + 1];
						y[i] = y[i + 1];
					}
					drawBufferPos--;
					
					idleCounter = 0;
				}
			}
			else
			{
				drawBufferPos = 0;
				lx = -1; ly = -1;
			}
		}
		nk_end(ctx);

		struct nk_panel tools;
		if (nk_begin(ctx, &tools, "Tools", nk_rect(0, 0, 200, pixelsHeight + 96),
			NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|
			NK_WINDOW_MINIMIZABLE|NK_WINDOW_TITLE))
		{
			nk_layout_row_dynamic(ctx, 20, 1);
			nk_label(ctx, "Brush Size:", NK_TEXT_LEFT);
			nk_layout_row_dynamic(ctx, 30, 1);
			nk_progress(ctx, &size, 50, 1);
			
			nk_layout_row_dynamic(ctx, 20, 1);
			nk_label(ctx, "Stabilization:", NK_TEXT_LEFT);
			nk_layout_row_dynamic(ctx, 30, 1);
			nk_progress(ctx, &drawBufferSize, MAX_DRAW_BUFFER_SIZE - 1, 1);
			if (drawBufferSize < 1)
				drawBufferSize = 1;
			if (drawBufferSize >= MAX_DRAW_BUFFER_SIZE)
				drawBufferSize = MAX_DRAW_BUFFER_SIZE - 1;

			nk_layout_row_dynamic(ctx, 20, 1);
			nk_label(ctx, "Foreground Color:", NK_TEXT_LEFT);
			nk_layout_row_dynamic(ctx, 120, 1);
			fg = nk_color_picker(ctx, fg, NK_RGBA);

			nk_layout_row_dynamic(ctx, 20, 1);
			nk_label(ctx, "Background Color:", NK_TEXT_LEFT);
			nk_layout_row_dynamic(ctx, 120, 1);
			bg = nk_color_picker(ctx, bg, NK_RGBA);

			nk_layout_row_dynamic(ctx, 30, 1);
			if (nk_button_label(ctx, "Foreground Fill", NK_BUTTON_DEFAULT))
				fillRect(0, 0, pixelsWidth, pixelsHeight, fg.r, fg.g, fg.b, fg.a);
			nk_layout_row_dynamic(ctx, 30, 1);
			if (nk_button_label(ctx, "Background Fill", NK_BUTTON_DEFAULT))
				fillRect(0, 0, pixelsWidth, pixelsHeight, bg.r, bg.g, bg.b, bg.a);
			if (fillUpdate())
			{
				nk_layout_row_dynamic(ctx, 20, 1);
				nk_label(ctx, "Filling in progress", NK_TEXT_LEFT);
			}
		}
		nk_end(ctx);

		glViewport(0, 0, w, h);
		glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		nk_glfw3_render(NK_ANTI_ALIASING_ON, 512 * 1024, 128 * 1024);
		glfwSwapBuffers(window);
	}
	nk_glfw3_shutdown();
	free(pixels);
	glfwTerminate();
	
	close(sockfd);
	return 0;
}

