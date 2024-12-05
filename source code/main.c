#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include "audio.h"
#include "physical.h"

#define BIRD_BODY_WIDTH 18
#define BIRD_BODY_HEIGHT 20
#define BIRD_HEAD_SIZE 15
#define BIRD_BEAK_SIZE 6
#define BIRD_COLOR 0xFFE0
#define VIDEO_BYTES 8         // Number of characters to read from /dev/video
#define PIPE_WIDTH 20         // Width of each pipe
#define MAX_PIPES 4           // Total number of pipes
#define GAP_SIZE 60           // Space for bird to pass through
#define MIN_PIPE_HEIGHT 100   // Minimum height for the top pipe section
#define MAX_PIPE_HEIGHT_DIFF 40 // Maximum allowed difference in height between pipes
#define FRAME_DELAY_NANOSECONDS 16666667  // Delay between frames (~50 ms for smooth animation)
#define SCROLL_SPEED_MULTIPLIER 10  // Use multiples of 10 for precision
#define SCROLL_SPEED 5.5              // 5 = 0.5 pixels per frame when divided by SCROLL_SPEED_MULTIPLIER
#define COMMAND_BUFFER_SIZE 2048
#define GRAVITY_MULTIPLIER 10
#define GRAVITY_SPEED 5        // 0.5 pixels per frame when divided by GRAVITY_MULTIPLIER
#define BOTTOM_MARGIN 1      // How far from bottom before stopping fall
#define JUMP_SPEED -2        // Negative because up is lower Y values
#define MAX_FALL_SPEED 2     // Maximum falling speed
#define GAME_OVER_X 52       // Adjusted for "GAME OVER" centering
#define RESTART_X 30         // Adjusted for "PRESS KEY1 to restart" centering
#define GAME_OVER_Y 35       // Middle of screen
#define RESTART_Y 40       // Line below game over
#define HEX_DEVICE "/dev/HEX"


// Structure to represent each pipe's position and dimensions
typedef struct {
    int x;          // X position of the pipe
    int top_height; // Height of the top section of the pipe
} Pipe;

typedef struct {
    int x;          // X position of bird's body left edge
    int y;          // Y position of bird's center
    int velocity;   // For later use with gravity
    float fall_accumulator;  // For smooth falling movement
} Bird;

Bird bird;

Pipe pipes[MAX_PIPES];
volatile sig_atomic_t stop = 0;  // Signal flag for Ctrl+C
int screen_x, screen_y;  // Variables for screen dimensions
float scroll_accumulator = 0.0f;
static char draw_command_buffer[COMMAND_BUFFER_SIZE];
static int draw_command_count = 0;
int score = 0;
int passed_pipes[MAX_PIPES] = {0};  // Track which pipes we've passed
int fd_hex;  // File descriptor for HEX device
int high_score = 0;
void* audio_virtual_base = NULL;
int fd = -1;  // File descriptor for /dev/mem

void catchSIGINT(int signum);
void initialize_pipes(void);
void safe_draw_box(int fd, int x1, int y1, int x2, int y2, short int color);
void flush_draw_commands(int fd);
void draw_pipe(int fd, Pipe pipe);
void update_and_draw_pipes(int fd);
void initialize_bird(void);
void draw_bird(int fd);
void update_bird(void);
int read_key_input(void);
int check_collision(void);
void restart_game(void);
void clear_text(int fd);
void display_game_over(int fd);

// Signal handler for SIGINT (Ctrl+C)
void catchSIGINT(int signum) {
    stop = 1;  // Set the flag to stop the game loop

    if (audio_virtual_base != NULL) {
        unmap_physical(audio_virtual_base, AUDIO_SPAN);
        audio_virtual_base = NULL;
    }

    if (fd != -1) {
        close_physical(fd);
        fd = -1;
    }
}



void initialize_bird() {
    // Position bird slightly to the right and in the middle of screen
    bird.x = screen_x / 3;
    bird.y = screen_y / 2;
    bird.velocity = 0;  // Start with no vertical velocity
    bird.fall_accumulator = 0.0f;
}

void display_on_hex(int fd, int value) {
    char buffer[20];
    snprintf(buffer, sizeof(buffer), "%06d\n", value);
    write(fd, buffer, strlen(buffer));
}
// Function to draw the bird
void draw_bird(int fd) {
    // Draw body (rectangle)
    safe_draw_box(fd, 
                 bird.x, 
                 bird.y - BIRD_BODY_HEIGHT/2,
                 bird.x + BIRD_BODY_WIDTH, 
                 bird.y + BIRD_BODY_HEIGHT/2,
                 BIRD_COLOR);

    // Draw head (square) - positioned at front of body
    safe_draw_box(fd,
                 bird.x + BIRD_BODY_WIDTH - BIRD_HEAD_SIZE/2,
                 bird.y - BIRD_BODY_HEIGHT/2 - BIRD_HEAD_SIZE/2,  // Position above body
                 bird.x + BIRD_BODY_WIDTH + BIRD_HEAD_SIZE/2,
                 bird.y - BIRD_BODY_HEIGHT/2 + BIRD_HEAD_SIZE/2,
                 BIRD_COLOR);

    // Draw beak (small square) - positioned at front of head
    safe_draw_box(fd,
                 bird.x + BIRD_BODY_WIDTH + BIRD_HEAD_SIZE/2,
                 bird.y - BIRD_BODY_HEIGHT/2 - BIRD_BEAK_SIZE/2,
                 bird.x + BIRD_BODY_WIDTH + BIRD_HEAD_SIZE/2 + BIRD_BEAK_SIZE,
                 bird.y - BIRD_BODY_HEIGHT/2 + BIRD_BEAK_SIZE/2,
                 BIRD_COLOR);
}

void clear_text(int fd) {
    char command[64];
    snprintf(command, sizeof(command), "erase\n");
    write(fd, command, strlen(command));
}

// Function to display game over text
void display_game_over(int fd) {
    char command[64];
    // Update high score if current score is higher
    if (score > high_score) {
        high_score = score;
    }

    snprintf(command, sizeof(command), "text %d,%d GAME OVER\n", 
             GAME_OVER_X, GAME_OVER_Y);
    write(fd, command, strlen(command));
    
    snprintf(command, sizeof(command), "text %d,%d PRESS KEY1 to restart\n", 
             RESTART_X, RESTART_Y);
    write(fd, command, strlen(command));
    // Add high score display (positioned 5 lines below restart text)
    snprintf(command, sizeof(command), "text %d,%d Highscore: %d\n", 
             RESTART_X - 12, RESTART_Y + 5, high_score);
    write(fd, command, strlen(command));
}

int read_key_input() {
    int fd = open("/dev/KEY", O_RDONLY);
    if (fd == -1) {
        perror("Failed to open KEY device");
        return 0;
    }
    char buffer[2];
    int bytes_read = read(fd, buffer, sizeof(buffer));
    close(fd);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        return (int)strtol(buffer, NULL, 16);
    }
    return 0;
}

void update_bird() {
	
    int key_input = read_key_input();
    if (key_input & 0x1) {  // KEY0 pressed
        // Move up 2 pixels immediately when button is pressed
        bird.y -= 6;
        // Reset fall accumulator to prevent immediate fall after jump
        bird.fall_accumulator = 0.0f;
    }
    // Update falling movement
    bird.fall_accumulator += (float)GRAVITY_SPEED / GRAVITY_MULTIPLIER;
    
    // Move bird down when we've accumulated enough for 1 pixel
    if (bird.fall_accumulator >= 1.0f) {
        int pixels_to_fall = (int)bird.fall_accumulator;
        bird.fall_accumulator -= pixels_to_fall;
        
        // Only update Y if not at bottom of screen
        if (bird.y + BIRD_BODY_HEIGHT/2 + pixels_to_fall < screen_y - BOTTOM_MARGIN) {
            bird.y += pixels_to_fall;
        }
    }
    // Keep bird within screen bounds
    if (bird.y - BIRD_BODY_HEIGHT/2 < 0) {
        bird.y = BIRD_BODY_HEIGHT/2;
    }
}


// Function to initialize pipes with constrained height differences
void initialize_pipes() {
    srand(time(NULL));
    int start_x = screen_x;  // Start pipes off the right edge of the screen
    int previous_height = MIN_PIPE_HEIGHT + rand() % (screen_y - GAP_SIZE - MIN_PIPE_HEIGHT);

    for (int i = 0; i < MAX_PIPES; i++) {
        pipes[i].x = start_x + i * (PIPE_WIDTH + 60);  // Space pipes evenly to the right of the screen
        
        // Randomize height with a max difference constraint
        int min_height = previous_height - MAX_PIPE_HEIGHT_DIFF;
        int max_height = previous_height + MAX_PIPE_HEIGHT_DIFF;

        if (min_height < MIN_PIPE_HEIGHT) min_height = MIN_PIPE_HEIGHT;
        if (max_height > screen_y - GAP_SIZE) max_height = screen_y - GAP_SIZE;

        pipes[i].top_height = min_height + rand() % (max_height - min_height + 1);
        previous_height = pipes[i].top_height;  // Update previous height for the next pipe
    }
}

// Function to safely draw a box within screen bounds
void safe_draw_box(int fd, int x1, int y1, int x2, int y2, short int color) {
    // Ensure x and y values are within bounds
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= screen_x) x2 = screen_x - 1;
    if (y2 >= screen_y) y2 = screen_y - 1;
   
    char command[64];
    int len = snprintf(command, sizeof(command), "box %d,%d %d,%d 0x%X\n", 
                      x1, y1, x2, y2, color);
    write(fd, command, len);
   
}

// Don't forget to flush any remaining commands at the end of drawing
void flush_draw_commands(int fd) {
    if (draw_command_count > 0) {
        write(fd, draw_command_buffer, draw_command_count);
        draw_command_count = 0;
    }
}
  
// Function to draw a single pipe with a top and bottom section
void draw_pipe(int fd, Pipe pipe) {
    // Draw top section of the pipe
    safe_draw_box(fd, pipe.x, 0, pipe.x + PIPE_WIDTH, pipe.top_height, 0x07E0);

    // Draw bottom section of the pipe, ensuring it doesn't exceed screen height
    int bottom_y_start = pipe.top_height + GAP_SIZE;
    if (bottom_y_start < screen_y) {
        safe_draw_box(fd, pipe.x, bottom_y_start, pipe.x + PIPE_WIDTH, screen_y - 1, 0x07E0);
    }
}

// Check if bird has collided with any pipe
int check_collision() {
    // Get bird boundaries considering head and beak
    int bird_left = bird.x;
    int bird_right = bird.x + BIRD_BODY_WIDTH + BIRD_HEAD_SIZE/2 + BIRD_BEAK_SIZE;
    int bird_top = bird.y - BIRD_BODY_HEIGHT/2 - BIRD_HEAD_SIZE/2;  // Consider head position
    int bird_bottom = bird.y + BIRD_BODY_HEIGHT/2;

    // Check collision with each pipe
    for (int i = 0; i < MAX_PIPES; i++) {
        // If bird is within pipe's x-range
        if (!(bird_right < pipes[i].x || bird_left > pipes[i].x + PIPE_WIDTH)) {
            // Check if bird hits top pipe or bottom pipe
            if (bird_top < pipes[i].top_height ||  // Hit top pipe
                bird_bottom > pipes[i].top_height + GAP_SIZE) {  // Hit bottom pipe
                return 1;  // Collision detected
            }
        }
    }
    return 0;  // No collision
}

// Function to restart the game
void restart_game() {
    pthread_mutex_lock(&audio_mutex); // Ensure no ongoing audio threads
    pthread_mutex_unlock(&audio_mutex);

    initialize_bird();
    
    // Reset pipes
    initialize_pipes();
    
    // Reset accumulators
    scroll_accumulator = 0.0f;

    score = 0;
    for (int i = 0; i < MAX_PIPES; i++) {
        passed_pipes[i] = 0;
    }
    display_on_hex(fd_hex, score);  // Reset HEX display when game restarts
    static int game_over_sound_played = 0; 
    game_over_sound_played = 0; // Reset the flag
}

void update_score() {
    for (int i = 0; i < MAX_PIPES; i++) {
        // Check if bird has fully passed this pipe and hasn't been counted yet
        if (!passed_pipes[i] && 
            bird.x > (pipes[i].x + PIPE_WIDTH)) {
            score++;
            passed_pipes[i] = 1;
	        start_coin_sound(audio_virtual_base);
        }
        
        // Reset passed flag when pipe STARTS to wrap around, not after it's completely off screen
        if (pipes[i].x >= screen_x) {  // When pipe resets to right side
            passed_pipes[i] = 0;  // Reset flag so we can count it again
        }
    }
}

// Function to update pipe positions and redraw them
void update_and_draw_pipes(int fd) {
    static int game_over = 0;  // Track game over state
    static int game_over_sound_played = 0; // Track if the sound was played

    
    draw_command_count = 0;
    write(fd, "clear\n", 6);
    write(fd, "sync\n", 5);
    if (game_over) {
        display_game_over(fd);
	// Play the "game over" sound only once
        if (!game_over_sound_played) {
            play_game_over_sound(audio_virtual_base);
            game_over_sound_played = 1; // Mark as played
        }
        
        int key_input = read_key_input();
        if (key_input & 0x2) {  // KEY1 pressed
            game_over = 0;
	    game_over_sound_played = 0;
            clear_text(fd);
            restart_game();
        }
        
        // Always end frame with sync and swap
        write(fd, "sync\n", 5);
        write(fd, "swap\n", 5);
        return;
    }
    
    // Move pipes left by SCROLL_SPEED pixels
    scroll_accumulator += (float)SCROLL_SPEED / SCROLL_SPEED_MULTIPLIER;
    
    // Only move pipes when we've accumulated at least 0.5 pixels of movement
    if (scroll_accumulator >= 1.0f) {
        int pixels_to_move = (int)scroll_accumulator;
        scroll_accumulator -= pixels_to_move;
        
        for (int i = 0; i < MAX_PIPES; i++) {
            pipes[i].x -= pixels_to_move;
            if (pipes[i].x + PIPE_WIDTH < 0) {
                pipes[i].x = screen_x;
                pipes[i].top_height = MIN_PIPE_HEIGHT + 
                    rand() % (screen_y - GAP_SIZE - MIN_PIPE_HEIGHT);
            }
        }
    }
    // Update bird position
    update_bird();
    update_score(); 
    // Check for collision
     if (check_collision()) {
        game_over = 1;
        display_game_over(fd);
        write(fd, "sync\n", 5);
        write(fd, "swap\n", 5);
        return;
    }

    // Draw all pipes
    for (int i = 0; i < MAX_PIPES; i++) {
        draw_pipe(fd, pipes[i]);
    }
    draw_bird(fd);

    flush_draw_commands(fd);
    
    write(fd, "sync\n", 5);
    
    write(fd, "swap\n", 5);
    
}

// Clear the entire screen (used on program exit)
//void clear_entire_screen(int fd) {
    // Clear both buffers
    //write(fd, "clear_all\n", 10);
//}




int main(int argc, char *argv[]) {
    int video_fd;
    char video_buffer[VIDEO_BYTES];
    struct timespec frame_time;
    frame_time.tv_sec = 0;
    frame_time.tv_nsec = FRAME_DELAY_NANOSECONDS;
    // Initialize audio
    fd = open_physical(fd);
    if (fd == -1){
        return -1;
    }
    // Map the audio base
    audio_virtual_base = map_physical(fd, AUDIO_BASE, AUDIO_SPAN);
    if (audio_virtual_base == NULL) {
        close_physical(fd);
        return EXIT_FAILURE;
    }  
    

    // Register signal handler for SIGINT
    signal(SIGINT, catchSIGINT);

    // Open the video device
    if ((video_fd = open("/dev/video", O_RDWR)) == -1) {
        perror("Error opening video device");
        return -1;
    }
    

    // Read screen dimensions from the driver
    if (read(video_fd, video_buffer, VIDEO_BYTES) == -1) {
        perror("Error reading from /dev/video");
        close(video_fd);
        return -1;
    }

    // Open HEX device
    fd_hex = open(HEX_DEVICE, O_WRONLY);
    if (fd_hex == -1) {
        perror("Error opening HEX device");
        close(video_fd);
        return -1;
    }
    sscanf(video_buffer, "%d %d", &screen_x, &screen_y);
    printf("Screen dimensions: %d x %d\n", screen_x, screen_y);
     // Clear screen initially
    write(video_fd, "clear\n", 6);
    write(video_fd, "sync\n", 5);
 
    
   
    // Initialize pipes with random positions and heights
    initialize_pipes();
    initialize_bird();

    // Animation loop: update and redraw pipes until interrupted
    printf("Starting main loop\n");
    while (!stop) {
	//printf("Score: %d\r", score);  // Print score and return to start of line
	fflush(stdout);  // Ensure score is displayed immediately
        update_and_draw_pipes(video_fd);  // Update positions and redraw pipes
	display_on_hex(fd_hex, score);  // Update HEX display with current score
	nanosleep(&frame_time, NULL);  // Maintain ~60 FPS
    }

    // Clear the screen before exiting
    clear_text(video_fd); 
    write(video_fd, "clear_both\n", 10);
    
    display_on_hex(fd_hex, 0);
    if (audio_virtual_base != NULL) {
        unmap_physical(audio_virtual_base, AUDIO_SPAN);
        audio_virtual_base = NULL;
    }

    if (fd != -1) {
        close_physical(fd);
        fd = -1;
    }
 
    close(video_fd);
    close(fd_hex);
    
    if (pthread_mutex_destroy(&audio_mutex) != 0) {
        perror("Failed to destroy mutex");
    }
  
    printf("Program terminated by user.\n");
    return 0;
}
