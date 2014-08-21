#include <pthread.h>
#include <assert.h>
#include <barrelfish/barrelfish.h>
#include <errno.h>
#include <string.h>

typedef void (*destructor_fn_t)(void *);
typedef void *(*start_fn_t)(void *);

struct   pthread_mutex_attr
{
  int pshared;
  int kind;
  int robustness;
};


struct pthread_mutex {
    struct thread_mutex mutex;
    int locked;
};

struct pthread_cond {
    struct thread_cond cond;
};

struct pthread {
    struct thread *thread;
    const void *keys[PTHREAD_KEYS_MAX];
    start_fn_t start_fn;
    void *arg;
    void *retval;
};

static pthread_key_t key_index = 0;
static struct thread_mutex key_mutex = THREAD_MUTEX_INITIALIZER;
static destructor_fn_t destructors[PTHREAD_KEYS_MAX];

static int start_pthread(void *arg)
{
    struct pthread *myself = arg;

    // Initialize TLS
    thread_set_tls_key(0, myself);

    // Run the thread
    myself->retval = myself->start_fn(myself->arg);

    // Call all key destructors
    for(pthread_key_t i = 0; i < key_index; i++) {
        if(destructors[i] != NULL) {
            destructors[i]((void *)myself->keys[i]);
        }
    }

    // 'myself' data structure is freed when joined with this thread
    return 0;
}

int pthread_create(pthread_t *pthread, const pthread_attr_t *attr,
                   void *(*start_routine) (void *), void *arg)
{
    *pthread = malloc(sizeof(struct pthread));
    assert(*pthread != NULL);
    memset(*pthread, 0, sizeof(struct pthread));

    // XXX: attributes are ignored.
    (*pthread)->start_fn = start_routine;
    (*pthread)->arg = arg;

    // Start the thread
    (*pthread)->thread = thread_create(start_pthread, *pthread);
    return 0;
}

pthread_t pthread_self(void)
{
    pthread_t self = thread_get_tls_key(0);

    // If NULL, we're the first thread, not created via pthreads.
    // Create a pthread structure.
    if(self == NULL) {
        struct pthread *myself = malloc(sizeof(struct pthread));
        assert(myself != NULL);
        memset(myself, 0, sizeof(struct pthread));
        myself->thread = thread_self();
        thread_set_tls_key(0, myself);
        self = myself;
    }

    return self;
}

void *pthread_getspecific(pthread_key_t key)
{
    if(key >= PTHREAD_KEYS_MAX) {
        return NULL;
    }

    return (void *)pthread_self()->keys[key];
}

int pthread_setspecific(pthread_key_t key, const void *val)
{
    if(key >= PTHREAD_KEYS_MAX) {
        return EINVAL;
    }

    pthread_self()->keys[key] = val;
    return 0;
}

int pthread_attr_init(pthread_attr_t *attr)
{
    // No attributes
    return 0;
}

static struct thread_mutex mutex_mutex = THREAD_MUTEX_INITIALIZER;

int pthread_mutex_init(pthread_mutex_t *mutex,
                       const pthread_mutexattr_t *attr)
{
    // XXX: Attributes ignored.
    *mutex = malloc(sizeof(struct pthread_mutex));
    if(*mutex == NULL) {
        return -1;
    }

    thread_mutex_init(&(*mutex)->mutex);
    (*mutex)->locked = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    if(*mutex != PTHREAD_MUTEX_INITIALIZER) {
        free(*mutex);
    }

    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    thread_mutex_lock(&mutex_mutex);

    if(*mutex == PTHREAD_MUTEX_INITIALIZER) {
        pthread_mutex_init(mutex, NULL);
    }

    (*mutex)->locked++;
    thread_mutex_unlock(&mutex_mutex);
    thread_mutex_lock(&(*mutex)->mutex);
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    thread_mutex_lock(&mutex_mutex);

    if(*mutex == PTHREAD_MUTEX_INITIALIZER) {
        pthread_mutex_init(mutex, NULL);
    }

    if((*mutex)->locked == 0) {
        thread_mutex_unlock(&mutex_mutex);
        return 0;
    }

    (*mutex)->locked--;
    thread_mutex_unlock(&mutex_mutex);
    thread_mutex_unlock(&(*mutex)->mutex);
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    thread_mutex_lock(&mutex_mutex);

    if(*mutex == PTHREAD_MUTEX_INITIALIZER) {
        pthread_mutex_init(mutex, NULL);
    }

    thread_mutex_unlock(&mutex_mutex);

    int retval = (thread_mutex_trylock(&(*mutex)->mutex) ? 0 : EBUSY);

    if(retval != EBUSY) {
        thread_mutex_lock(&mutex_mutex);
        (*mutex)->locked++;
        thread_mutex_unlock(&mutex_mutex);
    }

    return retval;
}

int pthread_cond_init(pthread_cond_t *cond,
			const pthread_condattr_t *attr)
{
    *cond = malloc(sizeof(struct pthread_cond));
    if(*cond == NULL) {
        return -1;
    }

    thread_cond_init(&(*cond)->cond);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    thread_mutex_lock(&mutex_mutex);
    if(*cond == PTHREAD_COND_INITIALIZER) {
        pthread_cond_init(cond, NULL);
    }
    thread_mutex_unlock(&mutex_mutex);

    thread_cond_signal(&(*cond)->cond);
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond,
                           pthread_mutex_t *mutex,
                           const struct timespec *timeout)
{
    thread_mutex_lock(&mutex_mutex);
    if(*cond == PTHREAD_COND_INITIALIZER) {
        pthread_cond_init(cond, NULL);
    }
    if(*mutex == PTHREAD_MUTEX_INITIALIZER) {
        pthread_mutex_init(mutex, NULL);
    }
    thread_mutex_unlock(&mutex_mutex);

    assert(!"NYI");
    return -1;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    thread_mutex_lock(&mutex_mutex);
    if(*cond == PTHREAD_COND_INITIALIZER) {
        pthread_cond_init(cond, NULL);
    }
    if(*mutex == PTHREAD_MUTEX_INITIALIZER) {
        pthread_mutex_init(mutex, NULL);
    }
    thread_mutex_unlock(&mutex_mutex);

    thread_cond_wait(&(*cond)->cond, &(*mutex)->mutex);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    thread_mutex_lock(&mutex_mutex);

    if(*cond == PTHREAD_COND_INITIALIZER) {
         pthread_cond_init(cond, NULL);
    }
    thread_mutex_unlock(&mutex_mutex);

    thread_cond_broadcast(&(*cond)->cond);

    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
    free(cond);
    return 0;
}

int pthread_join(pthread_t thread, void **retval)
{
    errval_t err = thread_join(thread->thread, NULL);
    assert(err_is_ok(err));

    *retval = thread->retval;
    free(thread);
    return 0;
}

int pthread_key_create(pthread_key_t *key,
                       void (*callback) (void *))
{
    int retval = 0;

    thread_mutex_lock(&key_mutex);

    if(key_index == PTHREAD_KEYS_MAX) {
        retval = EAGAIN;
        goto out;
    }

    *key = key_index;
    destructors[key_index] = callback;
    key_index++;

 out:
    thread_mutex_unlock(&key_mutex);
    return retval;
}

int pthread_key_delete(pthread_key_t key)
{
    thread_mutex_lock(&key_mutex);

    int result = EINVAL;

    if (key < PTHREAD_KEYS_MAX) {
        /* TODO: do something */
        result = 0;
    }

    thread_mutex_unlock(&key_mutex);
    return result;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
    int result = 0;
    pthread_mutexattr_t ma;

    ma = (pthread_mutexattr_t) calloc (1, sizeof (*ma));

    if (ma == NULL) {
        result = ENOMEM;
    }
    else {
        ma->pshared = PTHREAD_PROCESS_PRIVATE;
        ma->kind = PTHREAD_MUTEX_DEFAULT;
    }

    *attr = ma;

    return result;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
    int result = 0;

    if (attr == NULL || *attr == NULL) {
        result = EINVAL;
    } else {
        pthread_mutexattr_t ma = *attr;

        *attr = NULL;
        free (ma);
    }

    return result;
}

int pthread_mutexattr_getpshared(const pthread_mutexattr_t *attr,
                                 int *pshared)
{
    int result;

    if ((attr != NULL && *attr != NULL) && (pshared != NULL)) {
        *pshared = (*attr)->pshared;
        result = 0;
    } else {
        result = EINVAL;
    }

    return result;
}

int pthread_mutexattr_gettype(pthread_mutexattr_t *attr, int *type)
{
    int result = 0;

    if (attr != NULL && *attr != NULL && type != NULL) {
        *type = (*attr)->kind;
    } else {
        result = EINVAL;
    }

    return result;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{
    int result = 0;

    if ((attr != NULL && *attr != NULL)) {
        switch (type) {
            case PTHREAD_MUTEX_NORMAL:
            case PTHREAD_MUTEX_RECURSIVE:
            case PTHREAD_MUTEX_ERRORCHECK:
                (*attr)->kind = type;
                break;
            default:
                result = EINVAL;
                break;
        }
    } else {
        result = EINVAL;
    }

    return result;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int pshared)
{
    int result;

    if ((attr != NULL && *attr != NULL)
         && ((pshared == PTHREAD_PROCESS_SHARED)
                  || (pshared == PTHREAD_PROCESS_PRIVATE))) {

        if (pshared == PTHREAD_PROCESS_SHARED) {
#if !defined( _POSIX_THREAD_PROCESS_SHARED )
            result = ENOSYS;
            pshared = PTHREAD_PROCESS_PRIVATE;
#else
            result = 0;
#endif /* _POSIX_THREAD_PROCESS_SHARED */
        } else {
            result = 0;
        }

        (*attr)->pshared = pshared;
    } else {
        result = EINVAL;
    }

    return (result);
}



int pthread_equal(pthread_t pt1, pthread_t pt2)
{
    if (pt1 == NULL && pt2 == NULL) {
        return 1;
    }
    return pt1->thread == pt2->thread;
}
