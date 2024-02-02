#include <pthread.h>
#include <stdio.h>

#define THREADS 8
#define ITERATIONS 0x1000001ULL

/*
 * This is all pretty much directly from Linux's atomic helper functions, most
 * of it actually properly copied.
 */
#define RISCV_FENCE(p, s) \
        __asm__ __volatile__ ("fence " #p "," #s : : : "memory")

#ifdef MAGIC_FENCE
#define DO_MAGIC_FENCE() RISCV_FENCE(w, o)
#else
#define DO_MAGIC_FENCE() __asm__ volatile ("addi x0, x1, 9" ::: "memory")
#endif

#define READ_ONCE(x)  (*(const volatile typeof(x) *)&(x))

#define WRITE_ONCE(x, val)                                              \
do {                                                                    \
        *(volatile typeof(x) *)&(x) = (val);                            \
	DO_MAGIC_FENCE();						\
} while (0)

#define smp_load_acquire(p)                                             \
({                                                                      \
        typeof(*p) ___p1 = READ_ONCE(*p);                               \
        RISCV_FENCE(r,rw);                                              \
        ___p1;                                                          \
})

#define smp_store_release(p, v)                                         \
do {                                                                    \
        RISCV_FENCE(rw,w);                                              \
        WRITE_ONCE(*p, v);                                              \
} while (0)

#define smp_mb()      RISCV_FENCE(rw,rw)
#define smp_rmb()     RISCV_FENCE(r,r)
#define smp_wmb()     RISCV_FENCE(w,w)

#define wfe()			do {} while(0)
#define cpu_relax()		do {} while(0)
#define dsb_sev()		do {} while(0)
#define RISCV_RELEASE_BARRIER	"\tfence rw,  w\n"

#define xchg(ptr, new)   		                                \
({                                                                      \
        __typeof__(ptr) __ptr = (ptr);                                  \
        __typeof__(new) __new = (new);                                  \
        __typeof__(*(ptr)) __ret;                                       \
        switch (sizeof(*__ptr)) {                                       \
        case 4:                                                         \
                __asm__ __volatile__ (                                  \
                        "       amoswap.w.aqrl %0, %2, %1\n"            \
                        : "=r" (__ret), "+A" (*__ptr)                   \
                        : "r" (__new)                                   \
                        : "memory");                                    \
                break;                                                  \
        case 8:                                                         \
                __asm__ __volatile__ (                                  \
                        "       amoswap.d.aqrl %0, %2, %1\n"            \
                        : "=r" (__ret), "+A" (*__ptr)                   \
                        : "r" (__new)                                   \
                        : "memory");                                    \
                break;                                                  \
        }                                                               \
        __ret;                                                          \
})

#define smp_cond_load_relaxed(ptr, cond_expr) ({                \
        typeof(ptr) __PTR = (ptr);                              \
        typeof(*ptr) VAL;                                       \
        for (;;) {                                              \
                VAL = READ_ONCE(*__PTR);                        \
                if (cond_expr)                                  \
                        break;                                  \
                cpu_relax();                                    \
        }                                                       \
        (typeof(*ptr))VAL;                                      \
})

#define smp_cond_load_acquire(ptr, cond_expr) ({                \
        typeof(*ptr) _val;                                      \
        _val = smp_cond_load_relaxed(ptr, cond_expr);           \
        smp_acquire__after_ctrl_dep();                          \
        (typeof(*ptr))_val;                                     \
})

#define smp_acquire__after_ctrl_dep()             smp_rmb()

#define cmpxchg_release(ptr, old, new)                                  \
({                                                                      \
        __typeof__(ptr) __ptr = (ptr);                                  \
        __typeof__(*(ptr)) __old = (old);                               \
        __typeof__(*(ptr)) __new = (new);                               \
        __typeof__(*(ptr)) __ret;                                       \
        register unsigned int __rc;                                     \
        switch (sizeof(*__ptr)) {                                       \
        case 4:                                                         \
                __asm__ __volatile__ (                                  \
                        RISCV_RELEASE_BARRIER                           \
                        "0:     lr.w %0, %2\n"                          \
                        "       bne  %0, %z3, 1f\n"                     \
                        "       sc.w %1, %z4, %2\n"                     \
                        "       bnez %1, 0b\n"                          \
                        "1:\n"                                          \
                        : "=&r" (__ret), "=&r" (__rc), "+A" (*__ptr)    \
                        : "rJ" ((long)__old), "rJ" (__new)              \
                        : "memory");                                    \
                break;                                                  \
        case 8:                                                         \
                __asm__ __volatile__ (                                  \
                        RISCV_RELEASE_BARRIER                           \
                        "0:     lr.d %0, %2\n"                          \
                        "       bne %0, %z3, 1f\n"                      \
                        "       sc.d %1, %z4, %2\n"                     \
                        "       bnez %1, 0b\n"                          \
                        "1:\n"                                          \
                        : "=&r" (__ret), "=&r" (__rc), "+A" (*__ptr)    \
                        : "rJ" (__old), "rJ" (__new)                    \
                        : "memory");                                    \
                break;                                                  \
        }                                                               \
        __ret;                                                          \
})

/*
 * This is exactly Linux's MCS lock internals.
 */
struct mcs_spinlock {
        struct mcs_spinlock *next;
        int locked; /* 1 if lock acquired */
        int count;  /* nesting count, see qspinlock.c */
};

#ifndef arch_mcs_spin_lock_contended
/*
 * Using smp_cond_load_acquire() provides the acquire semantics
 * required so that subsequent operations happen after the
 * lock is acquired. Additionally, some architectures such as
 * ARM64 would like to do spin-waiting instead of purely
 * spinning, and smp_cond_load_acquire() provides that behavior.
 */
#define arch_mcs_spin_lock_contended(l)                                 \
do {                                                                    \
        smp_cond_load_acquire(l, VAL);                                  \
} while (0)
#endif

#ifndef arch_mcs_spin_unlock_contended
/*
 * smp_store_release() provides a memory barrier to ensure all
 * operations in the critical section has been completed before
 * unlocking.
 */
#define arch_mcs_spin_unlock_contended(l)                               \
        smp_store_release((l), 1)
#endif

/*
 * In order to acquire the lock, the caller should declare a local node and
 * pass a reference of the node to this function in addition to the lock.
 * If the lock has already been acquired, then this will proceed to spin
 * on this node->locked until the previous lock holder sets the node->locked
 * in mcs_spin_unlock().
 */
static inline
void mcs_spin_lock(struct mcs_spinlock **lock, struct mcs_spinlock *node)
{
        struct mcs_spinlock *prev;

        /* Init node */
        node->locked = 0;
        node->next   = NULL;

        /*
         * We rely on the full barrier with global transitivity implied by the
         * below xchg() to order the initialization stores above against any
         * observation of @node. And to provide the ACQUIRE ordering associated
         * with a LOCK primitive.
         */
        prev = xchg(lock, node);
        if (prev == NULL) {
                /*
                 * Lock acquired, don't need to set node->locked to 1. Threads
                 * only spin on its own node->locked value for lock acquisition.
                 * However, since this thread can immediately acquire the lock
                 * and does not proceed to spin on its own node->locked, this
                 * value won't be used. If a debug mode is needed to
                 * audit lock status, then set node->locked value here.
                 */
                return;
        }
        WRITE_ONCE(prev->next, node);

        /* Wait until the lock holder passes the lock down. */
        arch_mcs_spin_lock_contended(&node->locked);
}

/*
 * Releases the lock. The caller should pass in the corresponding node that
 * was used to acquire the lock.
 */
static inline
void mcs_spin_unlock(struct mcs_spinlock **lock, struct mcs_spinlock *node)
{
        struct mcs_spinlock *next = READ_ONCE(node->next);

        if (!next) {
                /*
                 * Release the lock by setting it to NULL
                 */
                if (cmpxchg_release(lock, node, NULL) == node)
                        return;
                /* Wait until the next pointer is set */
                while (!(next = READ_ONCE(node->next)))
                        cpu_relax();
        }

        /* Pass lock to next waiter. */
        arch_mcs_spin_unlock_contended(&next->locked);
}

/*
 * The actual test here, which essentially just spin lock/unlocks in a loop.
 */
struct mcs_spinlock *global;

struct thread_args {
	struct mcs_spinlock local_mcs_spinlock;
	struct mcs_spinlock *global_mcs_spinlock;
	int id;
};


void *thread(void *args_uncast) {
	struct thread_args *args = args_uncast;

	for (size_t i = 0; i < ITERATIONS; ++i) {
		mcs_spin_lock  (&args->global_mcs_spinlock, &args->local_mcs_spinlock);
		mcs_spin_unlock(&args->global_mcs_spinlock, &args->local_mcs_spinlock);
	}
}

void main() {
	pthread_t tid[THREADS];
	struct thread_args args[THREADS];

	global = NULL;

	for (size_t i = 0; i < THREADS; ++i) {
		args[i].global_mcs_spinlock = global;
		args[i].id = i;

		if (pthread_create(tid + i, NULL, &thread, args + i))
			perror("pthread_create()");
	}

	for (size_t i = 0; i < THREADS; ++i) {
		void *unused;
		if (pthread_join(tid[i], &unused))
			perror("pthread_join()");
	}
}
