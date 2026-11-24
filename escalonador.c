#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>      // sleep
#include <pthread.h>     // threads, mutex e variaveis de condicao POSIX

/* MAX_TASKS: capacidade da fila. Definida em tempo de compilacao.
 * Pode ser sobrescrita com: gcc -DMAX_TASKS=3 ... (o Makefile usa isso no
 * alvo run-cheia para forcar o bloqueio do produtor na demonstracao). */
#ifndef MAX_TASKS
#define MAX_TASKS 20
#endif

#define BYTES_POR_SEGUNDO 10      // buffer da tarefa = tempo * 10 bytes
#define TAM_LINHA         256     // tamanho do buffer de leitura de linha
#define TEMPO_MAXIMO      86400   // 24h; evita estouro em tempo * 10 e sleep absurdo
#define MAX_CONSUMIDORES  1024    // limite de sanidade para N

#define ID_FIM    (-1)            // id da tarefa especial "pilula de veneno"
/* A tarefa de fim recebe tempo INT_MAX => PIOR prioridade possivel.
 * Isso e essencial: se ela tivesse tempo 0, seria a de maior prioridade e os
 * consumidores encerrariam ANTES de processar as tarefas reais que ainda
 * estao na fila. Com INT_MAX, os N sinais de fim ficam sempre no final. */
#define TEMPO_FIM INT_MAX

/* ------------------------------------------------------------------------ */
/* Estruturas de dados (sempre alocadas dinamicamente)                       */
/* ------------------------------------------------------------------------ */

/* Uma tarefa. Cada tarefa e um no da lista ligada ordenada que implementa a
 * fila de prioridade (nao usamos nenhuma biblioteca de filas). */
typedef struct Tarefa
{
    int            id;         // identificador lido do arquivo (-1 = fim)
    int            tempo;      // tempo de execucao simulado, em segundos
    unsigned char *dados;      // buffer de tempo*10 bytes (zerado); NULL no fim
    struct Tarefa *prox;       // proximo no da lista (menor prioridade)
} Tarefa;

/* Fila de prioridade limitada, compartilhada por produtor e consumidores.
 * E a regiao critica do programa: todo acesso a cabeca/tamanho passa pelo
 * mutex. As duas variaveis de condicao substituem os semaforos vazio/cheio
 * do lab de produtor-consumidor. */
typedef struct
{
    Tarefa         *cabeca;     // tarefa de MAIOR prioridade (menor tempo)
    int             tamanho;    // quantas tarefas estao enfileiradas agora
    int             capacidade; // MAX_TASKS
    pthread_mutex_t mutex;      // exclusao mutua sobre a fila
    pthread_cond_t  naoCheia;   // produtor espera aqui quando a fila enche
    pthread_cond_t  naoVazia;   // consumidores esperam aqui quando esvazia
} FilaPrioridade;

/* Parametros do thread produtor (empacotados numa struct, como no cap. 7.4) */
typedef struct
{
    FilaPrioridade *fila;
    FILE           *arquivo;        // ja aberto pela main; o produtor fecha
    int             nConsumidores;  // quantos sinais de fim precisam ser enviados
} ParametrosProdutor;

/* ------------------------------------------------------------------------ */
/* Alocacao                                                                  */
/* ------------------------------------------------------------------------ */

/**
 * Aloca 'bytes' bytes ZERADOS ou aborta o programa.
 * Centraliza a checagem de erro de malloc (o lab repete esse if em cada
 * alocacao). Usa calloc para que o buffer da tarefa ja nasca "preenchido com
 * zeros", como pede o enunciado.
 * @param bytes quantidade de bytes
 * @param quem  descricao do que esta sendo alocado (para a mensagem de erro)
 */
static void *aloca(size_t bytes, const char *quem)
{
    void *p = calloc(1, bytes);
    if (p == NULL)
    {
        fprintf(stderr, "Problemas na alocacao de memoria para %s\n", quem);
        exit(EXIT_FAILURE);
    }
    return p;
}

/**
 * Cria uma tarefa: 1 malloc para a struct e 1 malloc para o buffer de dados
 * (tempo * 10 bytes). Tarefa de fim (id < 0) nao precisa de buffer.
 */
static Tarefa *criaTarefa(int id, int tempo)
{
    Tarefa *t = (Tarefa *) aloca(sizeof(Tarefa), "a estrutura da tarefa");
    t->id    = id;
    t->tempo = tempo;
    t->prox  = NULL;

    if (id < 0)
        t->dados = NULL;
    else
        /* tempo 0 => 0 bytes; calloc(1,0) pode devolver NULL em algumas libcs
         * e isso seria confundido com falha. Garantimos ao menos 1 byte. */
        t->dados = (unsigned char *)
            aloca((size_t) tempo * BYTES_POR_SEGUNDO + 1, "o buffer de dados da tarefa");
    return t;
}

/** Libera o buffer de dados e depois a estrutura (passo 5 do consumidor). */
static void liberaTarefa(Tarefa *t)
{
    if (t == NULL)
        return;
    free(t->dados);   // free(NULL) e seguro (caso da tarefa de fim)
    free(t);
}

/* ------------------------------------------------------------------------ */
/* Fila de prioridade limitada                                               */
/* ------------------------------------------------------------------------ */

static FilaPrioridade *criaFila(int capacidade)
{
    FilaPrioridade *f = (FilaPrioridade *) aloca(sizeof(FilaPrioridade), "a fila de prioridade");
    f->cabeca     = NULL;
    f->tamanho    = 0;
    f->capacidade = capacidade;

    if (pthread_mutex_init(&f->mutex, NULL) != 0 ||
        pthread_cond_init(&f->naoCheia, NULL) != 0 ||
        pthread_cond_init(&f->naoVazia, NULL) != 0)
    {
        fprintf(stderr, "Problemas na inicializacao do mutex/variaveis de condicao\n");
        exit(EXIT_FAILURE);
    }
    return f;
}

/** Destroi a fila. Se sobrou alguma tarefa (nao deveria), libera para nao vazar. */
static void destroiFila(FilaPrioridade *f)
{
    Tarefa *t = f->cabeca;
    while (t != NULL)
    {
        Tarefa *prox = t->prox;
        liberaTarefa(t);
        t = prox;
    }
    pthread_cond_destroy(&f->naoVazia);
    pthread_cond_destroy(&f->naoCheia);
    pthread_mutex_destroy(&f->mutex);
    free(f);
}

/**
 * Insere a tarefa na posicao correta da lista ordenada.
 * BLOQUEIA enquanto a fila estiver cheia (tamanho == MAX_TASKS).
 *
 * Ordem: menor tempo primeiro. Em empate, a nova tarefa vai DEPOIS de todas
 * as que ja tem o mesmo tempo (<=), o que preserva a ordem de chegada (FIFO).
 */
static void enfileira(FilaPrioridade *f, Tarefa *nova)
{
    pthread_mutex_lock(&f->mutex);

    /* while (e nao if): apos acordar, a condicao precisa ser testada de novo
     * (acordes espurios e corrida entre varios acordados). */
    while (f->tamanho == f->capacidade)
        pthread_cond_wait(&f->naoCheia, &f->mutex);

    if (f->cabeca == NULL || nova->tempo < f->cabeca->tempo)
    {
        // Fila vazia ou nova tarefa e a mais prioritaria: vira a cabeca.
        nova->prox = f->cabeca;
        f->cabeca  = nova;
    }
    else
    {
        // Anda ate o ultimo no com tempo <= nova->tempo e insere depois dele.
        Tarefa *atual = f->cabeca;
        while (atual->prox != NULL && atual->prox->tempo <= nova->tempo)
            atual = atual->prox;
        nova->prox  = atual->prox;
        atual->prox = nova;
    }
    f->tamanho++;

    pthread_cond_signal(&f->naoVazia);   // acorda UM consumidor
    pthread_mutex_unlock(&f->mutex);
}

/**
 * Remove e devolve a tarefa de maior prioridade (a cabeca da lista).
 * BLOQUEIA enquanto a fila estiver vazia.
 */
static Tarefa *desenfileira(FilaPrioridade *f)
{
    Tarefa *t;

    pthread_mutex_lock(&f->mutex);

    while (f->tamanho == 0)
        pthread_cond_wait(&f->naoVazia, &f->mutex);

    t         = f->cabeca;
    f->cabeca = t->prox;
    t->prox   = NULL;
    f->tamanho--;

    pthread_cond_signal(&f->naoCheia);   // abriu vaga: acorda o produtor
    pthread_mutex_unlock(&f->mutex);
    return t;
}

/* ------------------------------------------------------------------------ */
/* Threads                                                                   */
/* ------------------------------------------------------------------------ */

/**
 * Thread produtor.
 * Le o arquivo linha a linha ("id tempo"), cria a tarefa, enfileira e imprime.
 * Ao terminar o arquivo, enfileira N tarefas de fim (id = -1), uma por consumidor.
 *
 * @param args ParametrosProdutor*
 * @return int* (alocado com malloc) com o total de tarefas reais produzidas;
 *         quem recolhe no pthread_join e responsavel pelo free (mesmo padrao
 *         do mediaThreads.c do lab).
 */
static void *produtor(void *args)
{
    ParametrosProdutor *p = (ParametrosProdutor *) args;
    char *linha  = (char *) aloca(TAM_LINHA, "o buffer de leitura de linha");
    int  *total  = (int *)  aloca(sizeof(int), "o contador do produtor");
    int   nLinha = 0;
    int   i;

    while (fgets(linha, TAM_LINHA, p->arquivo) != NULL)
    {
        int id, tempo;
        int lidos = sscanf(linha, "%d %d", &id, &tempo);

        nLinha++;
        if (lidos == EOF)          // linha em branco: ignora em silencio
            continue;
        if (lidos != 2 || id < 0 || tempo < 0 || tempo > TEMPO_MAXIMO)
        {
            /* id < 0 e proibido na entrada: seria confundido com o sinal de
             * fim e derrubaria um consumidor antes da hora. */
            fprintf(stderr, "Produtor: linha %d invalida, ignorada.\n", nLinha);
            continue;
        }

        Tarefa *t = criaTarefa(id, tempo);
        enfileira(p->fila, t);     // pode bloquear aqui se a fila estiver cheia
        printf("Produtor: tarefa %d alocada e enfileirada.\n", id);
        fflush(stdout);
        (*total)++;
    }

    /* Fim do arquivo: um sinal de fim para cada consumidor. Como tem a pior
     * prioridade, so serao retirados depois de TODAS as tarefas reais. */
    for (i = 0; i < p->nConsumidores; i++)
        enfileira(p->fila, criaTarefa(ID_FIM, TEMPO_FIM));

    printf("Produtor: fim do arquivo, %d sinais de encerramento enfileirados.\n",
           p->nConsumidores);
    fflush(stdout);

    fclose(p->arquivo);
    free(linha);
    pthread_exit((void *) total);
}

/**
 * Thread consumidor (um por posicao do pool).
 * Laco infinito: retira a tarefa de maior prioridade, simula a execucao com
 * sleep(), libera a memoria. Sai do laco ao retirar uma tarefa com id < 0.
 *
 * @param args FilaPrioridade* compartilhada
 * @return int* (alocado com malloc) com quantas tarefas este consumidor executou.
 */
static void *consumidor(void *args)
{
    FilaPrioridade *fila = (FilaPrioridade *) args;
    unsigned long   tid  = (unsigned long) pthread_self();  // mesmo uso do thrd.c do lab
    int            *total = (int *) aloca(sizeof(int), "o contador do consumidor");

    while (1)
    {
        Tarefa *t = desenfileira(fila);          // 1. remove a de maior prioridade

        if (t->id < 0)                           // sinal de fim: encerra o laco
        {
            liberaTarefa(t);
            break;
        }

        printf("Consumidor %lu: iniciando tarefa %d (duracao %d s).\n",
               tid, t->id, t->tempo);            // 2.
        fflush(stdout);

        sleep((unsigned int) t->tempo);          // 3. simula a execucao

        printf("Consumidor %lu: tarefa %d concluida.\n", tid, t->id);  // 4.
        fflush(stdout);

        liberaTarefa(t);                         // 5. libera buffer e estrutura
        (*total)++;
    }

    pthread_exit((void *) total);
}

/* ------------------------------------------------------------------------ */
/* main                                                                      */
/* ------------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    FilaPrioridade     *fila;
    ParametrosProdutor *paramProd;
    pthread_t          *idProdutor;      // 1 posicao
    pthread_t          *idsConsumidores; // N posicoes, 1 unico malloc
    void               *retorno = NULL;
    FILE               *arquivo;
    char               *fim;
    long                n;
    int                 i, err;
    int                 produzidas, consumidas = 0;

    /* ---- Argumentos da linha de comando (nada e perguntado ao usuario) ---- */
    if (argc != 3)
    {
        fprintf(stderr, "Uso: %s <N consumidores> <arquivo de entrada>\n", argv[0]);
        return EXIT_FAILURE;
    }

    n = strtol(argv[1], &fim, 10);
    if (*fim != '\0' || n < 1 || n > MAX_CONSUMIDORES)
    {
        fprintf(stderr, "N deve ser um inteiro entre 1 e %d.\n", MAX_CONSUMIDORES);
        return EXIT_FAILURE;
    }

    arquivo = fopen(argv[2], "r");
    if (arquivo == NULL)
    {
        perror("Nao foi possivel abrir o arquivo de entrada");
        return EXIT_FAILURE;
    }

    /* ---- Alocacoes da main (cada estrutura em uma unica etapa) ---- */
    fila            = criaFila(MAX_TASKS);
    paramProd       = (ParametrosProdutor *) aloca(sizeof(ParametrosProdutor), "os parametros do produtor");
    idProdutor      = (pthread_t *) aloca(sizeof(pthread_t), "o id do produtor");
    idsConsumidores = (pthread_t *) aloca((size_t) n * sizeof(pthread_t), "os ids dos consumidores");

    paramProd->fila          = fila;
    paramProd->arquivo       = arquivo;     // a partir daqui o produtor e dono do FILE*
    paramProd->nConsumidores = (int) n;

    printf("Principal: %ld consumidores, fila de capacidade %d, arquivo '%s'.\n",
           n, MAX_TASKS, argv[2]);
    fflush(stdout);

    /* ---- Dispara o pool de consumidores e depois o produtor ---- */
    for (i = 0; i < n; i++)
    {
        err = pthread_create(&idsConsumidores[i], NULL, consumidor, (void *) fila);
        if (err != 0)
        {
            fprintf(stderr, "Erro na criacao do thread consumidor %d\n", i);
            exit(EXIT_FAILURE);
        }
    }

    err = pthread_create(idProdutor, NULL, produtor, (void *) paramProd);
    if (err != 0)
    {
        fprintf(stderr, "Erro na criacao do thread produtor\n");
        exit(EXIT_FAILURE);
    }

    /* ---- Juncao: o produtor termina primeiro (depois de enviar os N fins) ---- */
    err = pthread_join(*idProdutor, &retorno);
    if (err != 0)
    {
        fprintf(stderr, "Erro na juncao do thread produtor\n");
        exit(EXIT_FAILURE);
    }
    produzidas = *((int *) retorno);
    free(retorno);

    for (i = 0; i < n; i++)
    {
        err = pthread_join(idsConsumidores[i], &retorno);
        if (err != 0)
        {
            fprintf(stderr, "Erro na juncao do thread consumidor %d\n", i);
            exit(EXIT_FAILURE);
        }
        printf("Principal: consumidor %d (tid %lu) executou %d tarefa(s).\n",
               i, (unsigned long) idsConsumidores[i], *((int *) retorno));
        consumidas += *((int *) retorno);
        free(retorno);
    }

    printf("Principal: %d tarefas produzidas, %d executadas. Encerrando.\n",
           produzidas, consumidas);

    /* ---- Liberacao final: nada pode ficar alocado ---- */
    destroiFila(fila);
    free(idsConsumidores);
    free(idProdutor);
    free(paramProd);

    return (produzidas == consumidas) ? EXIT_SUCCESS : EXIT_FAILURE;
}
