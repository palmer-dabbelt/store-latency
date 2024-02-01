#include <pthread.h>
#include <stdio.h>

#define THREADS 8
#define ITERATIONS 0x10000ULL
#define MAGIC_FENCE

struct thread_args {
	unsigned id;
	unsigned long *shared;
};

static unsigned long global;

void *thread(void *args_uncast) {
	struct thread_args *args = args_uncast;

	for (size_t i = 0; i < ITERATIONS; ++i) {
		unsigned long cur;
		do {
			__asm__ volatile (
				"ld %0, 0(%1)"
				: "=r"(cur)
				: "r"(args->shared));
		} while ((cur % THREADS) != args->id);

		__asm__ volatile (
			"sd %0, 0(%1)"
			:
			: "r"(cur + 1), "r"(args->shared)
		);

#ifdef MAGIC_FENCE
		__asm__ volatile ("fence w,o");
#endif
	}
}

void main() {
	pthread_t tid[THREADS];
	struct thread_args args[THREADS];

	for (size_t i = 0; i < THREADS; ++i) {
		args[i].id   = i;
		args[i].shared = &global;

		if (pthread_create(tid + i, NULL, &thread, args + i))
			perror("pthread_create()");
	}

	for (size_t i = 0; i < THREADS; ++i) {
		void *unused;
		if (pthread_join(tid[i], &unused))
			perror("pthread_join()");
	}
}
