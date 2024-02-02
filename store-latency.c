#include <pthread.h>
#include <stdio.h>

#define THREADS 8
#define ITERATIONS 0x1000001ULL

struct thread_args {
	unsigned id;
	unsigned long *shared;
};

static unsigned long global;

void *thread(void *args_uncast) {
	struct thread_args *args = args_uncast;
	long locked;

	for (size_t i = 0; i < ITERATIONS; ++i) {
		unsigned long tmp;

		__asm__ volatile (
			"1:\n\t"
			"lr.w %[tmp], 0(%[mem])\n\t"
			"bne  %[tmp], %[ulv], 1b\n\t"
			"sc.w %[tmp], %[lkv], 0(%[mem])\n\t"
			"bnez %[tmp], 1b"
			: [tmp]"=&r"(tmp)
			: [mem]"r"(args->shared),
			  [ulv]"r"(0),
			  [lkv]"r"(locked)
		);

		__asm__ volatile (
			"sw %[ulv], 0(%[mem])"
			:
			: [ulv]"r"(0),
			  [mem]"r"(args->shared)
		);

#ifdef MAGIC_FENCE
		__asm__ volatile ("fence w,o");
#else
		__asm__ volatile ("addi x0, x0, 32");
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
