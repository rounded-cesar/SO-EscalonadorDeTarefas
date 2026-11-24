# Escalonador de Tarefas com Pool de Threads

Projeto prático da disciplina **TT304 – Sistemas Operacionais** (FT/UNICAMP).

Simula um escalonador: um thread **produtor** lê tarefas de um arquivo e as coloca em uma **fila de prioridade limitada**; um **pool de N threads consumidores** retira sempre a tarefa de menor tempo de execução e a "executa" com `sleep()`.

- Linguagem: **C puro** (sem C++), POSIX Threads, Linux
- Sincronização: `pthread_mutex_t` + `pthread_cond_t`
- Memória: tudo via `malloc`/`calloc`, nenhuma estrutura composta estática

## Autor

- Henrique César Mendonça Ituassu — RA 322436 — Sistemas de Informação


## Requisitos

Linux com `gcc` e `make`. Opcionais: `valgrind` (checagem de memória).

## Compilar

```bash
make
```

Equivale a:

```bash
gcc -std=gnu11 -Wall -Wextra -Wpedantic -O2 -pthread escalonador.c -o escalonador
```

## Executar

```bash
./escalonador <N consumidores> <arquivo de entrada>
./escalonador 3 tarefas.txt
```

Ou direto pelo Makefile:

| Comando | O que faz |
|---|---|
| `make run` | compila e roda com `N=3` e `tarefas.txt` |
| `make run N=5 ARQ=outro.txt` | roda com outros parâmetros |
| `make run-cheia N=1` | compila com `MAX_TASKS=3` e roda: dá para ver o produtor bloqueando com a fila cheia |
| `make run-grande` | gera `tarefas_grande.txt` (40 tarefas) e roda com 4 consumidores |
| `make valgrind` | roda sob Valgrind (vazamentos e acessos inválidos) |
| `make tsan` | roda com ThreadSanitizer (condições de corrida) |
| `make test` | `run` + `valgrind` + `tsan` |
| `make clean` | apaga binários e arquivos gerados |

A capacidade da fila é `MAX_TASKS` (padrão 20), definida em tempo de compilação: `gcc -DMAX_TASKS=50 ...`.

## Formato do arquivo de entrada

Uma tarefa por linha, dois inteiros: `id_tarefa tempo_execucao` (segundos).

```
1 3
2 1
3 5
```

- Linhas em branco são ignoradas.
- Linhas malformadas, com `id < 0` (reservado para o sinal de fim) ou com `tempo` fora de `0..86400` são ignoradas com aviso no `stderr`.
- Nenhum dado é pedido ao usuário durante a execução.

## Saída esperada

```
Produtor: tarefa <id> alocada e enfileirada.
Consumidor <tid>: iniciando tarefa <id> (duracao <t> s).
Consumidor <tid>: tarefa <id> concluida.
```

Ao final, a `main` mostra quantas tarefas cada consumidor executou e confere `produzidas == executadas`. O código de saída é `0` se tudo bateu.

## Como funciona (resumo)

- **Prioridade:** menor `tempo` = maior prioridade; empate = FIFO. A fila é uma lista ligada ordenada implementada à mão.
- **Fila cheia:** o produtor bloqueia em `pthread_cond_wait(&naoCheia)`. **Fila vazia:** consumidores bloqueiam em `pthread_cond_wait(&naoVazia)`.
- **Encerramento:** ao fim do arquivo o produtor enfileira N tarefas `id = -1` com `tempo = INT_MAX` (pior prioridade, então saem depois de todas as reais). Cada consumidor que retira uma sai do laço; a `main` faz `pthread_join` em todos e libera tudo.
- **Retorno de threads:** produtor e consumidores devolvem um `int*` alocado com `malloc` (contagem de tarefas), recolhido no `pthread_join` — mesmo padrão do `mediaThreads.c` do laboratório.

## Arquivos

| Arquivo | Conteúdo |
|---|---|
| `escalonador.c` | código-fonte completo e comentado |
| `Makefile` | compilação, execução e testes |
| `tarefas.txt` | entrada de exemplo (10 tarefas, com empates de tempo) |
| `README.md` | este arquivo |
