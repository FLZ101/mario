#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct NodeT {
  struct NodeT *prev, *next;
  int i;
} Node;

Node *createNode(int i) {
  Node *node = malloc(sizeof(Node));
  assert(node);

  node->prev = NULL;
  node->next = NULL;
  node->i = i;
  return node;
}

typedef struct {
  Node head, tail;
} List;

void initList(List *li) {
  li->head.prev = NULL;
  li->head.next = &li->tail;
  li->tail.prev = &li->head;
  li->tail.next = NULL;
  li->head.i = 0;
};

void prepend(List *li, int i) {
  Node *node = createNode(i);
  node->prev = &li->head;
  node->next = li->head.next;
  li->head.next->prev = node;
  li->head.next = node;
}

void append(List *li, int i) {
  Node *node = createNode(i);
  node->next = &li->tail;
  node->prev = li->tail.prev;
  li->tail.prev->next = node;
  li->tail.prev = node;
}

int empty(List *li) { return li->head.next == &li->tail; }

unsigned size(List *li) {
  unsigned n = 0;
  for (Node *node = li->head.next; node != &li->tail; ++n, node = node->next)
    ;
  return n;
}

void printList(List *li) {
  if (empty(li))
    return;

  Node *node = li->head.next;
  while (node != &li->tail) {
    if (node->prev != &li->head)
      printf(", ");
    printf("%d", node->i);

    node = node->next;
  }
  printf("\n");
}

void remove(Node *node) {
  node->prev->next = node->next;
  node->next->prev = node->prev;
  node->prev = NULL;
  node->next = NULL;

  free(node);
}

void freeList(List *li) {
  Node *node = li->head.next;
  while (node != &li->tail) {
    node = node->next;
    remove(node->prev);
  }
}

void test_linked_list() {
  puts("------ test_linked_list");

  printf("%x\n", sbrk(0));

  List li;
  initList(&li);
  for (int i = 0; i < 10; ++i) {
    if (i % 2)
      prepend(&li, i);
    else
      append(&li, i);
  }
  printf("size: %d\n", size(&li));

  printList(&li);

  freeList(&li);
  printf("size: %d\n", size(&li));

  printf("%x\n", sbrk(0));
}

// CHECK-NEXT: 0x00008000
// CHECK-NEXT: size: 10
// CHECK-NEXT: 9, 7, 5, 3, 1, 0, 2, 4, 6, 8
// CHECK-NEXT: size: 0
// CHECK-NEXT: 0x00008000
