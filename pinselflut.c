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

static int idleCounter = 0;
static void keepAlive()
{
	if (++idleCounter >= 60)
	{
		idleCounter = 0;
		const char nl = '\n';
		int n = write(sockfd, &nl, 1);
	}
}

static unsigned char buffer[1024], *p = buffer;
static void setPixel(int x, int y, struct nk_color color)
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
			idleCounter = 0;
		}
	}

	// add pixel to buffer
	*p++ = 'P'; *p++ = 'X'; *p++ = ' ';
	p += itoa(x, p); *p++ = ' ';
	p += itoa(y, p); *p++ = ' ';
	const unsigned char hex[] = "0123456789abcdef";
	*p++ = hex[color.r >> 4]; *p++ = hex[color.r & 0xf];
	*p++ = hex[color.g >> 4]; *p++ = hex[color.g & 0xf];
	*p++ = hex[color.b >> 4]; *p++ = hex[color.b & 0xf];
	*p++ = hex[color.a >> 4]; *p++ = hex[color.a & 0xf];
	*p++ = '\n';
	
	// set pixel locally
	float alpha = color.a / 255.0f, nalpha = 1.0f - alpha;
	uint8_t *pixel = pixels + (y * pixelsWidth + x) * 3;
	pixel[0] = (uint8_t)(pixel[0] * nalpha + color.r * alpha);
	pixel[1] = (uint8_t)(pixel[1] * nalpha + color.g * alpha);
	pixel[2] = (uint8_t)(pixel[2] * nalpha + color.b * alpha);
}

struct
{
	int x, y, w, h;
	struct nk_color color;
	int currentLine;
} fillState = {0};
static void fillRect(int x, int y, int w, int h, struct nk_color color)
{
	fillState.x = x; fillState.y = y; fillState.w = w; fillState.h = h;
	fillState.color = color;
	fillState.currentLine = 0;
}
static int fillUpdate()
{
	if (fillState.currentLine < fillState.h)
	{
		for (int x = 0; x < fillState.w; x++)
			setPixel(fillState.x + x, fillState.y + fillState.currentLine, fillState.color);
		fillState.currentLine++;
		return 1;
	}
	return 0;
}

typedef struct
{
	char name[64];
	struct nk_color color;
	nk_size size;
	nk_size stabilization;
} brush_t;
static void brushPoint(int x, int y, brush_t *brush)
{
	float radius = brush->size / 2.0f;
	x -= (int)ceilf(radius);
	y -= (int)ceilf(radius);
	float a2 = powf(brush->color.a / 255.0f, 1.0f / 5.0f);
	struct nk_color color = brush->color;
	for (int yi = 0; yi < brush->size + 1; yi++)
	{
		float dy = abs(yi - radius);
		for (int xi = 0; xi < brush->size + 1; xi++)
		{
			float dx = abs(xi - radius);
			float alpha = a2 * (1.0f - (sqrtf(dx * dx + dy * dy) / radius));
			if (alpha > 0.0f)
			{
				alpha = powf(alpha, 7.0f);
				alpha *= 255.0f;
				if (alpha >= 1.0f)
				{
					color.a = (uint8_t)alpha;
					setPixel(x + xi, y + yi, color);
				}
			}
		}
	}
}

static void brushLine(struct nk_vec2 p0, struct nk_vec2 p1, brush_t *brush)
{
	int x0 = (int)roundf(p0.x), y0 = (int)roundf(p0.y);
	int x1 = (int)roundf(p1.x), y1 = (int)roundf(p1.y);
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1; 
	int err = (dx > dy ? dx : -dy) / 2, e2;

	for(;;)
	{
		brushPoint(x0, y0, brush); // TODO: optimize number of sent pixels!
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
	#ifdef __APPLE__
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
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

	struct
	{
		#define MAX_STABILIZATION 32
		struct nk_vec2 positions[MAX_STABILIZATION];
		nk_size writeIndex;
		struct nk_vec2 lastAverage;
	} stabilizer = {0};
	stabilizer.lastAverage = nk_vec2(-1, -1);

	#define MAX_BRUSHES 32
	int brushCount = 2;
	brush_t brushes[MAX_BRUSHES] = {0};

	// default foreground brush
	strcpy(brushes[0].name, "Pen");
	brushes[0].color = nk_rgba(255, 255, 255, 255);
	brushes[0].size = 7;
	brushes[0].stabilization = 8;
	brush_t *fg = &brushes[0];

	// default background brush
	strcpy(brushes[1].name, "Erase");
	brushes[1].color = nk_rgba(0, 0, 0, 255);
	brushes[1].size = 15;
	brushes[1].stabilization = 1;
	brush_t *bg = &brushes[1];

	while (!glfwWindowShouldClose(window))
	{
		glfwPollEvents();
		int w, h;
		glfwGetFramebufferSize(window, &w, &h);
		nk_glfw3_new_frame();

		keepAlive();

		glBindTexture(GL_TEXTURE_2D, texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, pixelsWidth, pixelsHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels);
		struct nk_panel canvas;
		if (nk_begin(ctx, &canvas, "Canvas", nk_rect(200, 0, pixelsWidth + 32, pixelsHeight + 96),
			NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|//NK_WINDOW_SCALABLE|
			NK_WINDOW_MINIMIZABLE|NK_WINDOW_TITLE))
		{
			nk_layout_row_static(ctx, pixelsHeight, pixelsWidth, 1);
			struct nk_vec2 canvasPosition = nk_widget_position(ctx);
			nk_image(ctx, nk_image_id(texture));

			// brush strokes and stabilization
			if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS)
			{
				brush_t *brush = fg;
				if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS)
					brush = bg;

				stabilizer.positions[stabilizer.writeIndex] = nk_vec2(
					ctx->input.mouse.pos.x - canvasPosition.x,
					ctx->input.mouse.pos.y - canvasPosition.y);
				if (++stabilizer.writeIndex >= brush->stabilization)
				{
					float sumx = 0, sumy = 0;
					for (int i = 0; i < brush->stabilization; i++)
					{
						sumx += stabilizer.positions[i].x;
						sumy += stabilizer.positions[i].y;
					}
					struct nk_vec2 avg = nk_vec2(sumx / brush->stabilization, sumy / brush->stabilization);
					if (avg.x >= 0 && avg.y >= 0 && avg.x <= pixelsWidth - 1 && avg.y <= pixelsHeight - 1)
					{
						if (stabilizer.lastAverage.x == -1 && stabilizer.lastAverage.y == -1)
							stabilizer.lastAverage = avg;
						if ((int)roundf(avg.x) != (int)roundf(stabilizer.lastAverage.x) ||
							(int)roundf(avg.y) != (int)roundf(stabilizer.lastAverage.y))
							brushLine(stabilizer.lastAverage, avg, brush);
						stabilizer.lastAverage = avg;
					}
					
					// make room for next mouse position in stabilizer buffer
					for (int i = 0; i < stabilizer.writeIndex - 1; i++)
						stabilizer.positions[i] = stabilizer.positions[i + 1];
					stabilizer.writeIndex--;
				}
			}
			else
			{
				stabilizer.writeIndex = 0;
				stabilizer.lastAverage = nk_vec2(-1, -1);
			}
		}
		nk_end(ctx);

		struct nk_panel tools;
		if (nk_begin(ctx, &tools, "Tools", nk_rect(0, 0, 200, pixelsHeight + 96),
			NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|
			NK_WINDOW_MINIMIZABLE|NK_WINDOW_TITLE))
		{
			const char *brushNames[MAX_BRUSHES];
			int fgIndex = -1, bgIndex = -1;
			for (int i = 0; i < brushCount; i++)
			{
				brushNames[i] = brushes[i].name;
				if (brushes + i == fg) fgIndex = i;
				if (brushes + i == bg) bgIndex = i;
			}
			
			nk_layout_row_dynamic(ctx, 15, 1);
			nk_label(ctx, "Primary Brush:", NK_TEXT_LEFT);
			nk_layout_row_dynamic(ctx, 25, 1);
			fgIndex = nk_combo(ctx, brushNames, brushCount, fgIndex, 25);

			nk_layout_row_dynamic(ctx, 15, 1);
			nk_label(ctx, "Secondary Brush:", NK_TEXT_LEFT);
			nk_layout_row_dynamic(ctx, 25, 1);
			bgIndex = nk_combo(ctx, brushNames, brushCount, bgIndex, 25);

			nk_layout_row_dynamic(ctx, 15, 1); // empty
			nk_layout_row_dynamic(ctx, 15, 1);
			nk_label(ctx, "Brush Editor:", NK_TEXT_LEFT);
			int toDelete = -1;
			for (int i = 0; i < brushCount; i++)
			{
				brush_t *brush = brushes + i;
				if (nk_tree_push_id(ctx, NK_TREE_TAB, brush->name, NK_MINIMIZED, i))
				{
					nk_layout_row_dynamic(ctx, 25, 1);
					int len = strlen(brush->name);
					nk_edit_string(ctx, NK_EDIT_SIMPLE, brush->name, &len, sizeof(brush->name), nk_filter_default);
					brush->name[len] = 0;

					nk_layout_row_dynamic(ctx, 15, 1);
					nk_label(ctx, "Brush Size:", NK_TEXT_LEFT);
					nk_layout_row_dynamic(ctx, 20, 1);
					nk_progress(ctx, &brush->size, 50, 1);
					if (brush->size < 1)
						brush->size = 1;

					nk_layout_row_dynamic(ctx, 15, 1);
					nk_label(ctx, "Stabilization:", NK_TEXT_LEFT);
					nk_layout_row_dynamic(ctx, 20, 1);
					nk_progress(ctx, &brush->stabilization, MAX_STABILIZATION - 1, 1);
					if (brush->stabilization < 1)
						brush->stabilization = 1;
					if (brush->stabilization >= MAX_STABILIZATION)
						brush->stabilization = MAX_STABILIZATION - 1;

					nk_layout_row_dynamic(ctx, 120, 1);
					brush->color = nk_color_picker(ctx, brush->color, NK_RGBA);

					nk_layout_row_dynamic(ctx, 20, 1);
					if (nk_button_label(ctx, "Fill Canvas", NK_BUTTON_DEFAULT))
						fillRect(0, 0, pixelsWidth, pixelsHeight, brush->color);
					
					if (brushCount > 1)
					{
						nk_layout_row_dynamic(ctx, 20, 1);
						if (nk_button_label(ctx, "Delete Brush", NK_BUTTON_DEFAULT))
							toDelete = i;
					}
					
					nk_tree_pop(ctx);
				}
			}

			if (brushCount < MAX_BRUSHES)
			{
				nk_layout_row_dynamic(ctx, 20, 1);
				if (nk_button_label(ctx, "Add Brush", NK_BUTTON_DEFAULT))
				{
					strcpy(brushes[brushCount].name, "New Brush");
					brushes[brushCount].size = 1;
					brushes[brushCount].stabilization = 1;
					brushes[brushCount].color = nk_rgba(255, 255, 255, 255);
					fgIndex = brushCount;
					brushCount++;
				}
			}
			
			if (toDelete >= 0)
			{
				brushCount--;
				for (int i = toDelete; i < brushCount; i++)
					brushes[i] = brushes[i + 1];
				if (fgIndex >= toDelete) fgIndex--;
				if (fgIndex < 0) fgIndex = 0;
				if (bgIndex >= toDelete) bgIndex--;
				if (bgIndex < 0) bgIndex = 0;
			}

			fg = brushes + fgIndex;
			bg = brushes + bgIndex;

			if (fillUpdate())
			{
				nk_layout_row_dynamic(ctx, 15, 1);
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

