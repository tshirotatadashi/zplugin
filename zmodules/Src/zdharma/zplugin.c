/* -*- Mode: C; c-default-style: "linux"; c-basic-offset: 4; indent-tabs-mode: nil -*-
 * vim:sw=4:sts=4:et
 *
 * zplugin.c – module for Zplugin plugin manager
 *
 * Copyright (c) 2017 Sebastian Gniazdowski
 * All rights reserved.
 */

#define fdheaderlen(f) (((Wordcode) (f))[FD_PRELEN])
#define fdbyte(f,i)      ((wordcode) (((unsigned char *) (((Wordcode) (f)) + 1))[i]))
#define fdflags(f)       fdbyte(f, 0)
#define fdhtail(f)       (((FDHead) (f))->flags >> 2)
#define fdother(f)       (fdbyte(f, 1) + (fdbyte(f, 2) << 8) + (fdbyte(f, 3) << 16))
#define fdmagic(f)       (((Wordcode) (f))[0])
#define fdversion(f)     ((char *) ((f) + 2))
#define firstfdhead(f) ((FDHead) (((Wordcode) (f)) + FD_PRELEN))
#define nextfdhead(f)  ((FDHead) (((Wordcode) (f)) + (f)->hlen))
#define fdname(f)      ((char *) (((FDHead) (f)) + 1))

#define FD_PRELEN 12
#define FD_MAGIC  0x04050607
#define FD_OMAGIC 0x07060504

#define FDF_MAP   1
#define FDF_OTHER 2

#define THREAD_ERROR -1
#define THREAD_INITIAL 0
#define THREAD_READY 1
#define THREAD_WORKING 2
#define THREAD_FINISHED 3
#define THREAD_OUT_CONSUMED 4

typedef unsigned int wordcode;
typedef wordcode *Wordcode;

typedef struct fdhead *FDHead;

struct fdhead {
    wordcode start;		/* offset to function definition */
    wordcode len;		/* length of wordcode/strings */
    wordcode npats;		/* number of patterns needed */
    wordcode strs;		/* offset to strings */
    wordcode hlen;		/* header length (incl. name) */
    wordcode flags;		/* flags and offset to name tail */
};

#include "zplugin.mdh"
#include "zplugin.pro"
#include <unistd.h>
#include <pthread.h>

static HandlerFunc originalAutoload = NULL;

struct prepare_node {
    struct hashnode node;

    pthread_t thread;
    Eprog prog;
    volatile int state;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int retval;
};

typedef struct prepare_node *PrepareNode;

/* Maps file paths to structure holding state of turbo-Eprog */
static HashTable prepare_hash = NULL;

/* ARRAY: builtin {{{ */
static struct builtin bintab[] =
{
    BUILTIN("source_prepare", 0, bin_source_prepare, 1, 1, 0, "", NULL),
    BUILTIN("source_load", 0, bin_source_load, 1, 1, 0, "", NULL),
};
/* }}} */

static const char *out_hash = "ZPLG_FBODIES";

/* FUNCTION: bin_autoload2 {{{ */
/*
 * k   - ksh autoload
 * z   - zsh autoload
 * X,- - replace definition of function with autoloaded one, execute
 * X,+ - load immediately, do not execute
 * m   - name treated as pattern, used with +X
 * d   - search $fpath even with a path
 * U   - no aliases
 * W   - WARN_NESTED_VAR
 * r   - resolve absolute path, silent on error
 * R   - resolve absolute path, aborts on error
 * t   - turn on execution tracing
 * T   - as above, but turn off for called functions
 */

/**/
static int
bin_autoload2(char *name, char **argv, Options ops, int func)
{
    Shfunc shf;
    int on = 0, off = 0, roff;
    char **in_argv = argv;

    /* Do we have any flags defined? */
    if (OPT_ISSET(ops,'X'))
        on |= PM_UNDEFINED;
    if (OPT_MINUS(ops,'U'))
        on |= PM_UNALIASED|PM_UNDEFINED;
    else if (OPT_PLUS(ops,'U'))
        off |= PM_UNALIASED;
    if (OPT_MINUS(ops,'t'))
        on |= PM_TAGGED;
    else if (OPT_PLUS(ops,'t'))
        off |= PM_TAGGED;
    if (OPT_MINUS(ops,'T'))
        on |= PM_TAGGED_LOCAL;
    else if (OPT_PLUS(ops,'T'))
        off |= PM_TAGGED_LOCAL;
    if (OPT_MINUS(ops,'W'))
        on |= PM_WARNNESTED;
    else if (OPT_PLUS(ops,'W'))
        off |= PM_WARNNESTED;
    roff = off;
    if (OPT_MINUS(ops,'z')) {
        on |= PM_ZSHSTORED;
        off |= PM_KSHSTORED;
    } else if (OPT_PLUS(ops,'z')) {
        off |= PM_ZSHSTORED;
        roff |= PM_ZSHSTORED;
    }
    if (OPT_MINUS(ops,'k')) {
        on |= PM_KSHSTORED;
        off |= PM_ZSHSTORED;
    } else if (OPT_PLUS(ops,'k')) {
        off |= PM_KSHSTORED;
        roff |= PM_KSHSTORED;
    }
    if (OPT_MINUS(ops,'d')) {
        on |= PM_CUR_FPATH;
        off |= PM_CUR_FPATH;
    } else if (OPT_PLUS(ops,'d')) {
        off |= PM_CUR_FPATH;
        roff |= PM_CUR_FPATH;
    }

    /* If option recognizing is finished */
    int finished = 0;

    if ( (off & PM_UNDEFINED) || (OPT_ISSET(ops,'k') && OPT_ISSET(ops,'z')) ||
         (OPT_MINUS(ops,'X') && (OPT_ISSET(ops,'m') || !scriptname)) )
        {
            /**
             ** This path is: invalid options
             **/

            zwarnnam( name, "Invalid option(s)" );
            finished = 1;
        }

    if ( !finished && OPT_MINUS( ops,'X' ) ) {
        /**
         ** This path is: eval autoload
         % calendar() { autoload -X; }
         % calendar
         calendar:autoload: My: -X
         calendar:calendar: My: eval_autoload does -X, calls mkautofn, bin_eval
        **/

        zwarnnam( name, "-X path" );
        finished = 1;
    } else if ( !finished && !*argv) {
        /**
         ** This path is: print functions
         **/

        zwarnnam( name, "print functions" );
        finished = 1;
    }

    if ( !finished && OPT_ISSET( ops,'m' ) ) {
        /**
         ** This path is: apply options to matching functions
         **               or print functions if no options given
         autoload: -m
         calendar: check_autoload has seen X
         calendar: eval_autoload does loadautofn
         zsh: loadautofn
        **/

        zwarnnam( name, "print functions / apply options, to matching" );
        finished = 1;
    }

    if ( ! finished ) {
        /* Take the arguments literally -- do not glob */
        for (; *argv; argv++) {
            if (OPT_ISSET(ops,'w')) {
                /**
                 ** This path is: from-zcompile
                 **/
                // returnval = dump_autoload(name, *argv, on, ops, func);

                zwarnnam( name, "dump_autoload" );
                finished = 1;
            } else if ((shf = (Shfunc) shfunctab->getnode(shfunctab, *argv))) {
                if (on|off) {
                    /**
                     ** This path is: update options
                     **/

                    /**
                     ** This path is: change options, handle +X
                     % autoload calendar
                     autoload: & PM_UNDEFINED
                     autoload: add_autoload_function
                     autoload: Third check_autoload
                     % autoload +X calendar
                     autoload: Update options
                     autoload: First check_autoload
                     calendar: check_autoload has seen X
                     calendar: eval_autoload does loadautofn
                     zsh: loadautofn
                    **/
                    /* turn on/off the given flags */
                    // shf->node.flags = (shf->node.flags | (on & ~PM_UNDEFINED)) & ~off;
                    // if (check_autoload(shf, shf->node.nam, ops, func))
                    //     returnval = 1;

                    finished = 1;
                } else {
                    /**
                     ** This path is: print function
                     **/

                    finished = 1;
                }
            } else if (on & PM_UNDEFINED) {
                /**
                 ** This path is: +X or no -/+X
                 **/

                if ( **argv == '/' ) {
                    /**
                     ** This path is: update options, update path, handle +X
                     % autoload calendar
                     autoload: & PM_UNDEFINED
                     autoload: add_autoload_function
                     autoload: Third check_autoload
                     % autoload +X /usr/local/share/zsh/5.3.1-dev-0/functions/calendar
                     autoload: & PM_UNDEFINED
                     autoload: At == /
                     autoload: Second check_autoload
                     calendar: check_autoload has seen X
                     calendar: eval_autoload does loadautofn
                     zsh: loadautofn
                    **/

                    char *base = strrchr(*argv, '/') + 1;
                    if ( *base && ( shf = (Shfunc) shfunctab->getnode( shfunctab, base ) ) ) {
                        /* turn on/off the given flags */
                        // shf->node.flags = (shf->node.flags | (on & ~PM_UNDEFINED)) & ~off;
                        if (shf->node.flags & PM_UNDEFINED) {
                            /* update path if not yet loaded */
                            // dircache_set(&shf->filename, NULL);
                            // dircache_set(&shf->filename, dir);
                        }
                        // if (check_autoload(shf, shf->node.nam, ops, func))
                        //  returnval = 1;

                        finished = 1;
                        continue;
                    }
                }

                /**
                 ** This path is: add autoload function STUB
                 % autoload +X calendar
                 autoload: & PM_UNDEFINED
                 autoload: add_autoload_function
                 autoload: Third check_autoload
                 calendar: check_autoload has seen X
                 calendar: eval_autoload does loadautofn
                 zsh: loadautofn
                **/
                // if (ok && check_autoload(shf, shf->node.nam, ops, func))
                //    returnval = 1;

                zwarnnam( name, "Adding autoload stub" );
                finished = 1;
            } else {
                /**
                 ** This path is: bad arguments
                 **/
                // returnval = 1;

                zwarnnam( name, "bad arguments" );
                finished = 1;
            }
        }
    }

    return originalAutoload( name, in_argv, ops, func );
}
/* }}} */
/* FUNCTION: bin_source_prepare {{{ */

/**/
static int
bin_source_prepare(char *name, char **argv, Options ops, int func)
{
    PrepareNode pn;
    char *zwc_path;
    int reusing = 0;

    zwc_path = *argv;

    if (!zwc_path) {
        zwarnnam(name, "Argument required - path to .zwc file, which is to be prepared");
        return 1;
    }

    /* Only three possible cases to reuse prepare-request:
     * - thread is fresh, fully unused,
     * - thread has finished, and its data has been already read
     * - no thread created because of an error */
    if((pn = (PrepareNode) gethashnode2(prepare_hash, zwc_path))) {
        if (pn->state != THREAD_INITIAL && pn->state != THREAD_OUT_CONSUMED && pn->state != THREAD_ERROR) {
            zwarnnam(name, "The file provided is being already turbo-loaded");
            return 1;
        }
        reusing = 1;
    }

    /* Allocate if no reused node */
    if (!pn) {
        pn = (PrepareNode) zshcalloc(sizeof(struct prepare_node));
    }

    if (pn) {
        pn->prog = NULL;
        pn->state = THREAD_READY;
        pn->thread = NULL;
        pn->retval = 0;
        if (!reusing) {
            addhashnode(prepare_hash, ztrdup(zwc_path), (void *)pn);
        }
    } else {
        zwarnnam(name, "Out of memory when allocating load-Eprog task");
        return 1;
    }

    /* We use mutex to be sure, that the thread has ran */
    pthread_cond_init( &pn->cond, NULL );
    pthread_mutex_init( &pn->mutex, NULL );
    pthread_mutex_lock( &pn->mutex );
    /* It will try locking at end of task, and will wait
     * for source_load to unlock the mutex */

    if (pthread_create(&pn->thread, NULL, background_load, (void*)pn)) {
        zwarnnam(name, "Error creating thread");
        pn->prog = NULL;
        pn->state = THREAD_ERROR;
    }

    return 0;
}
/* }}} */
/* FUNCTION: bin_source_load {{{ */

/**/
static int
bin_source_load(char *name, char **argv, Options ops, int func)
{
    PrepareNode pn;
    char *zwc_path;
    zwc_path = *argv;

    if (!zwc_path) {
        zwarnnam(name, "Argument required - path to .zwc file, which is to be loaded (after source_prepare)");
        return 1;
    }

    if(!(pn = (PrepareNode) gethashnode2(prepare_hash, zwc_path))) {
        zwarnnam(name, "The file wasn't passed to source_prepare, aborting");
        return 1;
    }

    /* Wait for signal about load finishing */
    pthread_cond_wait( &pn->cond, &pn->mutex );
    pthread_mutex_unlock( &pn->mutex );
    /* Cleanup */
    pthread_mutex_destroy( &pn->mutex );
    pthread_cond_destroy( &pn->cond );

    if (pn->state != THREAD_FINISHED) {
        zwarnnam(name, "source_prepared finished preparing, but state is inconsistent, aborting");
        return 1;
    }

    Eprog prog = pn->prog;
    execode(prog, 1, 0, "filecode");

    pn->state = THREAD_OUT_CONSUMED;

    return 0;
}
/* }}} */
/* FUNCTION: background_load {{{ */

/**/
static
void *background_load( void *void_ptr )
{
    PrepareNode pn = (PrepareNode) void_ptr;
    pn->retval = 0;

    pn->state = THREAD_WORKING;
    
    char *file = pn->node.nam;
    Eprog result = try_zwc_file( file );
    if (!result) {
        fprintf(stderr, "source_prepare failed to load %s in background, aborting\n", file);
        fflush(stderr);

        /* Lock mutex, signal other thread that load is finished */
        pthread_mutex_lock( &pn->mutex );
        pthread_cond_signal( &pn->cond );
        pthread_mutex_unlock( &pn->mutex );

        pn->retval = 1;

        pthread_exit(&pn->retval);
        return &pn->retval;
    }

    pn->state = THREAD_FINISHED;
    pn->prog = result;

    /* Lock mutex, signal other thread that load is finished */
    pthread_mutex_lock( &pn->mutex );
    pthread_cond_signal( &pn->cond );
    pthread_mutex_unlock( &pn->mutex );

    pthread_exit(&pn->retval);
    return &pn->retval;
}
/* }}} */
/* FUNCTION: try_zwc_file {{{ */

/**/
Eprog
try_zwc_file(char *file)
{
    Eprog prog;
    char *tail;

    /* Name of .zwc script */
    if ((tail = strrchr(file, '/'))) {
	tail++;
    } else {
	tail = file;
    }

    /* Check for .zwc at input */
    if (!strsfx(".zwc", file)) {
        zwarn("Turbo-source applies only to .zwc file, please provide path to such file");
	return NULL;
    }

    /* Load byte-code */
    if ((prog = load_zwc(file, tail))) {
	return prog;
    }

    return NULL;
}
/* }}} */
/* FUNCTION: load_zwc {{{ */

/**/
static Eprog
load_zwc(char *file, char *name2)
{
    Wordcode d = NULL;
    FDHead h;

    /* Load heder */
    if (!(d = load_dheader(NULL, file, 0))) {
	return NULL;
    }

    char *name = strdup(name2);
    char *found = strstr(name, ".zwc");
    *found = '\0';

    /* Check for file name to exist as function */
    if ((h = find_in_header(d, name))) {
        Eprog prog;
        Patprog *pp;
        int np, fd, po = h->npats * sizeof(Patprog);

        if ((fd = open(file, O_RDONLY)) < 0 ||
                lseek(fd, ((h->start * sizeof(wordcode)) +
                        ((fdflags(d) & FDF_OTHER) ? fdother(d) : 0)), 0) < 0)
        {
            zwarn("Turbo-source: Failed to open file with byte-code (.zwc extension), consider compiling the file");
            if (fd >= 0)
                close(fd);
            return NULL;
        }

        /* Func exists, file opens -> can read */
        d = (Wordcode) zalloc(h->len + po);

        if (read(fd, ((char *) d) + po, h->len) != (int)h->len) {
            zwarn("Turbo-source couldn't load byte-code, after correct open()");
            close(fd);
            zfree(d, h->len);
            return NULL;
        }
        close(fd);

        prog = (Eprog) zalloc(sizeof(*prog));

        prog->flags = EF_REAL;
        prog->len = h->len + po;
        prog->npats = np = h->npats;
        prog->nref = 1; /* allocated from permanent storage */
        prog->pats = pp = (Patprog *) d;
        prog->prog = (Wordcode) (((char *) d) + po);
        prog->strs = ((char *) prog->prog) + h->strs;
        prog->shf = NULL;
        prog->dump = NULL;

        while (np--)
            *pp++ = dummy_patprog1;

        return prog;
    }

    return NULL;
}
/* }}} */
/* FUNCTION: load_dheader {{{ */

/**/
static Wordcode
load_dheader(char *nam, char *name, int err)
{
    int fd, v = 1;
    wordcode buf[FD_PRELEN + 1];

    if ((fd = open(name, O_RDONLY)) < 0) {
	if (err)
	    zwarnnam(nam, "can't open zwc file: %s", name);
	return NULL;
    }
    if (read(fd, buf, (FD_PRELEN + 1) * sizeof(wordcode)) !=
	((FD_PRELEN + 1) * sizeof(wordcode)) ||
	(v = (fdmagic(buf) != FD_MAGIC && fdmagic(buf) != FD_OMAGIC))) {
	if (err) {
	    if (!v) {
		zwarnnam(nam, "zwc file has wrong version (zsh-%s): %s",
			 fdversion(buf), name);
	    } else
		zwarnnam(nam, "invalid zwc file: %s" , name);
	}
	close(fd);
	return NULL;
    } else {
	int len;
	Wordcode head;

	if (fdmagic(buf) == FD_MAGIC) {
	    len = fdheaderlen(buf) * sizeof(wordcode);
	    head = (Wordcode) zhalloc(len);
	}
	else {
	    int o = fdother(buf);

	    if (lseek(fd, o, 0) == -1 ||
		read(fd, buf, (FD_PRELEN + 1) * sizeof(wordcode)) !=
		((FD_PRELEN + 1) * sizeof(wordcode))) {
		zwarnnam(nam, "invalid zwc file: %s" , name);
		close(fd);
		return NULL;
	    }
	    len = fdheaderlen(buf) * sizeof(wordcode);
	    head = (Wordcode) zhalloc(len);
	}
	memcpy(head, buf, (FD_PRELEN + 1) * sizeof(wordcode));

	len -= (FD_PRELEN + 1) * sizeof(wordcode);
	if (read(fd, head + (FD_PRELEN + 1), len) != len) {
	    close(fd);
	    zwarnnam(nam, "invalid zwc file: %s" , name);
	    return NULL;
	}
	close(fd);
	return head;
    }
}
/* }}} */
/* FUNCTION: find_in_header {{{ */

/**/
static FDHead
find_in_header(Wordcode h, char *name)
{
    FDHead n, e = (FDHead) (h + fdheaderlen(h));

    for (n = firstfdhead(h); n < e; n = nextfdhead(n))
	if (!strcmp(name, fdname(n) + fdhtail(n)))
	    return n;

    return NULL;
}
/* }}} */
/* FUNCTION: set_length {{{ */
/*
 * For zsh-allocator, rest of Zsh seems to use
 * free() instead of zsfree(), and such length
 * restoration causes slowdown, but all is this
 * way strict - correct */
/**/
static void set_length(char *buf, int size) {
    buf[size]='\0';
    while (-- size >= 0) {
        buf[size]=' ';
    }
}
/* }}} */
/* FUNCTION: createhashtable {{{ */

/**/
static HashTable
createhashtable(char *name)
{
    HashTable ht;

    ht = newhashtable(8, name, NULL);

    ht->hash        = hasher;
    ht->emptytable  = emptyhashtable;
    ht->filltable   = NULL;
    ht->cmpnodes    = strcmp;
    ht->addnode     = addhashnode;
    ht->getnode     = gethashnode2;
    ht->getnode2    = gethashnode2;
    ht->removenode  = removehashnode;
    ht->disablenode = NULL;
    ht->enablenode  = NULL;
    ht->freenode    = freepreparenode;
    ht->printnode   = NULL;

    return ht;
}
/* }}} */
/* FUNCTION: freepreparenode {{{ */

/**/
static void
freepreparenode(HashNode hn)
{
    zsfree(hn->nam);
    zfree(hn, sizeof(struct prepare_node));
}
/* }}} */

static struct features module_features =
{
    bintab, sizeof(bintab)/sizeof(*bintab),
    NULL, 0,
    NULL, 0,
    NULL, 0,
    0
};

/**/
int
setup_(UNUSED(Module m))
{
    // Builtin bn = (Builtin) builtintab->getnode2(builtintab, "autoload");
    // originalAutoload = bn->handlerfunc;
    // bn->handlerfunc = bin_autoload2;

    /* Create private preparing-hash */
    if (!(prepare_hash = createhashtable("prepare_hash"))) {
        zwarn("Cannot create backend-register hash");
        return 1;
    }

    return 0;
}

/**/
int
features_(Module m, char ***features)
{
    *features = featuresarray(m, &module_features);
    return 0;
}

/**/
int
enables_(Module m, int **enables)
{
    return handlefeatures(m, &module_features, enables);
}

/**/
int
boot_(Module m)
{
    return 0;
}

/**/
int
cleanup_(Module m)
{
    return setfeatureenables(m, &module_features, NULL);
}

/**/
int
finish_(UNUSED(Module m))
{
    // Builtin bn = (Builtin) builtintab->getnode2(builtintab, "autoload");
    // bn->handlerfunc = originalAutoload;

    printf("zdharma/zplugin module unloaded\n");
    fflush(stdout);
    return 0;
}
