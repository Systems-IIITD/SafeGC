#include <stdio.h> 
#include <stdlib.h>
#include <assert.h>
#include "memory.h"

#define MAX_NODES 100000
#define MAX_EDGES 50
#define MAX_REPLACEMENTS 1000000

struct node {
	int head;
	struct node* edges[];
};

struct wrapper {
	int not_used;
	struct node n;
};


typedef struct node* Node;

static int num_nodes = MAX_NODES;
static int num_edges = MAX_EDGES;
static int num_replacements = MAX_REPLACEMENTS;
static int padding = MAX_EDGES * sizeof(Node);

Node allocate_n()
{
	struct wrapper *w = (struct wrapper*)mymalloc(sizeof(struct wrapper) + padding);
	if (w == NULL)
	{
		printf("unable to allocate new node\n");
		exit(0);
	}
	w->n.head = 0;
	return &w->n;
}

void substitute(Node cur_n, Node old_n, Node new_n)
{
	int head_local = cur_n->head;
	int i;

	for (i = 0; i < head_local; i++)
	{
		if (cur_n->edges[i] == old_n)
		{
			cur_n->edges[i] = new_n;
		}
	}
}

void replace_with(Node old_n, Node new_n)
{
	int i;
	Node n;

	for (i = 0; i < old_n->head; i++)
	{
		new_n->edges[i] = old_n->edges[i];
		old_n->edges[i] = NULL;
	}
	new_n->head = old_n->head;

	for (i = 0; i < new_n->head; i++)
	{
		substitute(new_n->edges[i], old_n, new_n);
	}
}

void replace(Node nodes[])
{
	int i = rand() % num_nodes;
	Node original = nodes[i];
	Node new_n = allocate_n();
	replace_with(original, new_n);
	nodes[i] = new_n;
}

static void interconnects__inner(Node nodes[], int src_idx)
{
	int edges = rand() % num_edges;
	int i, dst_idx;
	Node src = nodes[src_idx];
	Node dst;

	for (i = 0; i <= edges; i++)
	{
		dst_idx = rand() % num_nodes;
		dst = nodes[dst_idx];
		if (src->head == num_edges || dst->head == num_edges || src == dst)
			continue;
		src->edges[src->head++] = dst;
		dst->edges[dst->head++] = src;
	}
}

static void interconnects(Node nodes[])
{
	for (int i = 0; i < num_nodes; i++)
	{
		interconnects__inner(nodes, i);
	}
}

int main(int argc, char *argv[])
{
	int i;
	unsigned long long total_edges = 0;
	assert(argc <= 4);

	if (argc >= 2) {
		num_nodes = atoi(argv[1]);
		assert(num_nodes > 0 && num_nodes <= MAX_NODES);
	}
	if (argc >= 3) {
		num_edges = atoi(argv[2]);
		assert(num_edges > 0 && num_edges <= MAX_EDGES);
		padding = num_edges * sizeof(Node);
	}
	if (argc == 4) {
		num_replacements = atoi(argv[3]);
		assert(num_replacements > 0 && num_replacements <= MAX_REPLACEMENTS);
	}

	Node *nodes = mymalloc(sizeof(Node) * num_nodes);
	if (nodes == NULL)
	{
		printf("unable to allocate nodes array!\n");
		return 0;
	}

	for (i = 0; i < num_nodes; i++)
	{
		nodes[i] = allocate_n();
	}

	interconnects(nodes);

	for (int i = 0; i < num_replacements; i++)
	{
		replace(nodes);
	}

	for (i = 0; i < num_nodes; i++)
	{
		assert(nodes[i]);
		total_edges += nodes[i]->head;
		nodes[i] = NULL;
	}
	nodes = NULL;
	printf("total edges:%lld\n", total_edges);
	printMemoryStats();
	asm volatile("xor %%rbp, %%rbp \n\t" 
							 "xor %%rbx, %%rbx \n\t"
							 "xor %%r12, %%r12 \n\t"
							 "xor %%r13, %%r13 \n\t"
							 "xor %%r14, %%r14 \n\t"
							 "xor %%r15, %%r15 \n\t": : : 
							 "%rbp", "%rbx", "%r12", "%r13", "%r14", "%r15");
	
	runGC();
	printf("printing stats after final GC\n");
	printMemoryStats();

	return 0;
}
