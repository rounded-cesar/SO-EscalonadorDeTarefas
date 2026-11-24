# Makefile - Escalonador de tarefas (TT304 - Sistemas Operacionais)
#
# Uso rapido:
#   make                 compila
#   make run             compila e executa com N=3 e tarefas.txt
#   make run N=5 ARQ=tarefas_grande.txt   muda os parametros
#   make run-cheia       fila de capacidade 3 (mostra o produtor bloqueando)
#   make run-grande      40 tarefas geradas automaticamente
#   make valgrind        checa vazamentos / acessos invalidos
#   make tsan            checa condicoes de corrida (ThreadSanitizer)
#   make clean           apaga binarios

CC      ?= gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -Wpedantic -O2 -pthread
N       ?= 3
ARQ     ?= tarefas.txt

BIN        := escalonador
BIN_CHEIA  := escalonador_cheia
BIN_DBG    := escalonador_dbg
BIN_TSAN   := escalonador_tsan

.PHONY: all run run-cheia run-grande valgrind tsan test clean

all: $(BIN)

$(BIN): escalonador.c
	$(CC) $(CFLAGS) $< -o $@

# Mesma fonte, mas com MAX_TASKS=3 para o produtor bloquear logo (demo do video).
$(BIN_CHEIA): escalonador.c
	$(CC) $(CFLAGS) -DMAX_TASKS=3 $< -o $@

# Build de depuracao (sem otimizacao e com simbolos) para o valgrind.
$(BIN_DBG): escalonador.c
	$(CC) -std=gnu11 -Wall -Wextra -g -O0 -pthread $< -o $@

$(BIN_TSAN): escalonador.c
	$(CC) -std=gnu11 -Wall -Wextra -g -O1 -pthread -fsanitize=thread $< -o $@

tarefas_grande.txt:
	awk 'BEGIN { for (i = 1; i <= 40; i++) print i, (i * 7) % 4 + 1 }' > $@

run: $(BIN)
	./$(BIN) $(N) $(ARQ)

run-cheia: $(BIN_CHEIA)
	./$(BIN_CHEIA) $(N) $(ARQ)

run-grande: $(BIN) tarefas_grande.txt
	./$(BIN) 4 tarefas_grande.txt

valgrind: $(BIN_DBG)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 \
	         ./$(BIN_DBG) $(N) $(ARQ)

tsan: $(BIN_TSAN)
	./$(BIN_TSAN) $(N) $(ARQ)

# Roda tudo que importa para a entrega.
test: run valgrind tsan

clean:
	rm -f $(BIN) $(BIN_CHEIA) $(BIN_DBG) $(BIN_TSAN) tarefas_grande.txt
