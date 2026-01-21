#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#define NUM_TRADERS 3
#define BUFFER_SIZE 10
#define INITIAL_BALANCE 10000.0

// TODO: Definiáld a Transaction struktúrát (láncolt lista)
// Tartalmazzon: type, stock, quantity, price, next pointer
typedef struct Transaction {
    char type[10];           // "VETEL" vagy "ELADAS"
    char stock[10];          // Részvény neve
    int quantity;            // Mennyiség
    double price;            // Ár
    struct Transaction *next;
} Transaction;

// TODO: Definiáld a StockPrice struktúrát
// Tartalmazzon: stock név, price
typedef struct {
    char stock[10];
    double price;
} StockPrice;

// TODO: Globális változók
// - price_buffer tömb
// - buffer_count, buffer_read_idx, buffer_write_idx
// - wallet_balance, stocks_owned
// - mutex-ek (wallet, buffer, transaction)
// - condition variable
// - transaction_head pointer
// - running flag (volatile sig_atomic_t)
// - market_pid
StockPrice price_buffer[BUFFER_SIZE];
int buffer_count = 0;
int buffer_read_idx = 0;
int buffer_write_idx = 0;

double wallet_balance = INITIAL_BALANCE;
int stocks_owned = 0;

pthread_mutex_t wallet_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t buffer_cond = PTHREAD_COND_INITIALIZER;

Transaction *transaction_head = NULL;
pthread_mutex_t transaction_mutex = PTHREAD_MUTEX_INITIALIZER;

volatile sig_atomic_t running = 1;
pid_t market_pid = -1;

// TODO: Implementáld az add_transaction függvényt
// malloc-al foglalj memóriát, töltsd ki a mezőket
// mutex lock alatt add hozzá a láncolt lista elejéhez
void add_transaction(const char *type, const char *stock, int quantity, double price) {
    Transaction *trans = (Transaction *)malloc(sizeof(Transaction));
    if (!trans) return;
    
    strcpy(trans->type, type);
    strcpy(trans->stock, stock);
    trans->quantity = quantity;
    trans->price = price;
    
    pthread_mutex_lock(&transaction_mutex);
    trans->next = transaction_head;
    transaction_head = trans;
    pthread_mutex_unlock(&transaction_mutex);
}

// TODO: Implementáld a print_transactions függvényt
// Járd végig a láncolt listát mutex lock alatt
// Írd ki az összes tranzakciót
void print_transactions() {
    pthread_mutex_lock(&transaction_mutex);
    printf("\n========== TRANZAKCIOS NAPLO ==========\n");
    Transaction *current = transaction_head;
    int count = 0;
    while (current) {
        printf("%d. %s: %d db %s @ %.2f $\n", 
               ++count, current->type, current->quantity, 
               current->stock, current->price);
        current = current->next;
    }
    printf("=======================================\n");
    pthread_mutex_unlock(&transaction_mutex);
}

// TODO: Implementáld a free_transactions függvényt
// FONTOS: Járd végig a listát és free()-zd az összes elemet
// Ez kell a Valgrind tiszta kimenethez!
void free_transactions() {
    pthread_mutex_lock(&transaction_mutex);
    Transaction *current = transaction_head;
    while (current) {
        Transaction *temp = current;
        current = current->next;
        free(temp);
    }
    transaction_head = NULL;
    pthread_mutex_unlock(&transaction_mutex);
}

// TODO: Signal handler (SIGINT)
// Állítsd be a running flag-et 0-ra
// Küldj SIGTERM-et a market_pid folyamatnak (kill függvény)
// Ébreszd fel a szálakat (pthread_cond_broadcast)
void signal_handler(int sig) {
    if (sig == SIGINT) {
        printf("\n[SIGNAL] Leallitas indul...\n");
        running = 0;
        
        // Piac folyamat leallitasa
        if (market_pid > 0) {
            kill(market_pid, SIGTERM);
        }
        
        // Szalak felepitese
        pthread_cond_broadcast(&buffer_cond);
    }
}

// TODO: Piac folyamat függvénye
// Végtelen ciklusban:
// - Generálj random részvénynevet és árat
// - Írás a pipe_fd-re (write)
// - sleep(1)
void market_process(int pipe_fd) {
    const char *stocks[] = {"AAPL", "GOOG", "TSLA", "MSFT"};
    int num_stocks = 4;
    
    srand(time(NULL) ^ getpid());
    
    while (1) {
        char message[50];
        int stock_idx = rand() % num_stocks;
        double price = 100.0 + (rand() % 200);
        
        snprintf(message, sizeof(message), "%s %.2f\n", stocks[stock_idx], price);
        
        if (write(pipe_fd, message, strlen(message)) < 0) {
            break;
        }
        
        sleep(1);
    }
    
    close(pipe_fd);
    exit(0);
}

// TODO: Kereskedő szál függvénye
// Végtelen ciklusban:
// - pthread_cond_wait amíg buffer_count == 0
// - Kivesz egy árfolyamot a bufferből (mutex alatt!)
// - Kereskedési döntés (random vagy stratégia)
// - wallet_balance módosítása (MUTEX!!!)
// - add_transaction hívás
void *trader_thread(void *arg) {
    int trader_id = *(int *)arg;
    free(arg);
    
    srand(time(NULL) ^ pthread_self());
    
    while (running) {
        pthread_mutex_lock(&buffer_mutex);
        
        // Várakozás új árfolyamra
        while (buffer_count == 0 && running) {
            pthread_cond_wait(&buffer_cond, &buffer_mutex);
        }
        
        if (!running) {
            pthread_mutex_unlock(&buffer_mutex);
            break;
        }
        
        // Árfolyam kivétele a bufferből
        StockPrice price = price_buffer[buffer_read_idx];
        buffer_read_idx = (buffer_read_idx + 1) % BUFFER_SIZE;
        buffer_count--;
        
        pthread_mutex_unlock(&buffer_mutex);
        
        // Kereskedési döntés (egyszerű stratégia)
        int decision = rand() % 100;
        
        pthread_mutex_lock(&wallet_mutex);
        
        if (decision < 40 && wallet_balance >= price.price) {
            // VÉTEL
            int quantity = 1 + (rand() % 3);
            double cost = quantity * price.price;
            
            if (wallet_balance >= cost) {
                wallet_balance -= cost;
                stocks_owned += quantity;
                printf("[Trader %d] VETEL: %d db %s @ %.2f $ (Egyenleg: %.2f $)\n",
                       trader_id, quantity, price.stock, price.price, wallet_balance);
                add_transaction("VETEL", price.stock, quantity, price.price);
            }
        } else if (decision >= 60 && stocks_owned > 0) {
            // ELADÁS
            int quantity = 1 + (rand() % (stocks_owned < 3 ? stocks_owned : 3));
            double revenue = quantity * price.price;
            
            wallet_balance += revenue;
            stocks_owned -= quantity;
            printf("[Trader %d] ELADAS: %d db %s @ %.2f $ (Egyenleg: %.2f $)\n",
                   trader_id, quantity, price.stock, price.price, wallet_balance);
            add_transaction("ELADAS", price.stock, quantity, price.price);
        }
        
        pthread_mutex_unlock(&wallet_mutex);
    }
    
    return NULL;
}

int main() {
    int pipe_fd[2];
    pthread_t traders[NUM_TRADERS];
    
    printf("========================================\n");
    printf("  WALL STREET - PARHUZAMOS TOZSDE\n");
    printf("========================================\n");
    printf("Kezdo egyenleg: %.2f $\n", INITIAL_BALANCE);
    printf("Kereskedok szama: %d\n", NUM_TRADERS);
    printf("Ctrl+C a leallitashoz\n");
    printf("========================================\n\n");
    
    // TODO: Signal handler regisztrálása
    signal(SIGINT, signal_handler);
    
    // TODO: Pipe létrehozása
    if (pipe(pipe_fd) == -1) {
        perror("pipe");
        exit(1);
    }
    
    // TODO: Fork - Piac folyamat indítása
    // Ha gyerek (== 0): piac folyamat
    // Ha szülő: kereskedő szálak indítása
    market_pid = fork();
    if (market_pid == -1) {
        perror("fork");
        exit(1);
    }
    
    if (market_pid == 0) {
        // Gyerek: Piac
        close(pipe_fd[0]); // Olvasó vég bezárása
        market_process(pipe_fd[1]);
    }
    
    // Szülő: Master
    close(pipe_fd[1]); // Író vég bezárása
    
    // TODO: Kereskedő szálak indítása (pthread_create)
    // for ciklus, malloc az id-nak
    for (int i = 0; i < NUM_TRADERS; i++) {
        int *trader_id = (int *)malloc(sizeof(int));
        *trader_id = i + 1;
        if (pthread_create(&traders[i], NULL, trader_thread, trader_id) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }
    
    // TODO: Master ciklus
    // Olvasd a pipe-ot (read)
    // Parse-old az árakat
    // Tedd be a bufferbe (mutex alatt!)
    // pthread_cond_broadcast
    char buffer[256];
    size_t buffer_pos = 0;
    
    while (running) {
        char ch;
        ssize_t bytes_read = read(pipe_fd[0], &ch, 1);
        
        if (bytes_read <= 0) {
            break;
        }
        
        if (ch == '\n') {
            buffer[buffer_pos] = '\0';
            
            // Árfolyam parse-olása
            char stock[10];
            double price;
            if (sscanf(buffer, "%s %lf", stock, &price) == 2) {
                pthread_mutex_lock(&buffer_mutex);
                
                // Buffer tele van?
                while (buffer_count >= BUFFER_SIZE && running) {
                    pthread_mutex_unlock(&buffer_mutex);
                    usleep(10000);
                    pthread_mutex_lock(&buffer_mutex);
                }
                
                if (running) {
                    strcpy(price_buffer[buffer_write_idx].stock, stock);
                    price_buffer[buffer_write_idx].price = price;
                    buffer_write_idx = (buffer_write_idx + 1) % BUFFER_SIZE;
                    buffer_count++;
                    
                    pthread_cond_broadcast(&buffer_cond);
                }
                
                pthread_mutex_unlock(&buffer_mutex);
            }
            
            buffer_pos = 0;
        } else if (buffer_pos < sizeof(buffer) - 1) {
            buffer[buffer_pos++] = ch;
        }
    }
    
    close(pipe_fd[0]);
    
    // TODO: Cleanup
    // pthread_join a szálakra
    // waitpid a Piac folyamatra
    // Végső kiírások
    // free_transactions()
    // mutex destroy
    for (int i = 0; i < NUM_TRADERS; i++) {
        pthread_join(traders[i], NULL);
    }
    
    // Piac folyamat várása
    if (market_pid > 0) {
        waitpid(market_pid, NULL, 0);
    }
    
    // Végső eredmények
    printf("\n========================================\n");
    printf("           VEGLEGES EGYENLEG\n");
    printf("========================================\n");
    printf("Penzegeszles: %.2f $\n", wallet_balance);
    printf("Reszveny keszlet: %d db\n", stocks_owned);
    printf("========================================\n");
    
    print_transactions();
    
    free_transactions();
    pthread_mutex_destroy(&wallet_mutex);
    pthread_mutex_destroy(&buffer_mutex);
    pthread_mutex_destroy(&transaction_mutex);
    pthread_cond_destroy(&buffer_cond);
    
    printf("\n[RENDSZER] Sikeres leallitas.\n");
    return 0;
}
