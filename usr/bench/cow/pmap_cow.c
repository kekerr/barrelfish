#include <barrelfish/barrelfish.h>
#include <barrelfish/except.h>
#include <barrelfish_kpi/paging_target.h>
#include <assert.h>

#include "pmap_cow.h"
#include "debug.h"

static struct vnode *cow_root_pte = NULL;
#define EX_STACK_SIZE 16384
static char ex_stack[EX_STACK_SIZE];

static errval_t alloc_vnode_noalloc(struct pmap_x86 *pmap, struct vnode *root,
                     struct capref vnodecap, uint32_t entry,
                     struct vnode **retvnode)
{
    errval_t err;

    struct vnode *newvnode = slab_alloc(&pmap->slab);
    if (newvnode == NULL) {
        return LIB_ERR_SLAB_ALLOC_FAIL;
    }
    newvnode->u.vnode.cap = vnodecap;

    // Map it
    err = vnode_map(root->u.vnode.cap, newvnode->u.vnode.cap, entry,
                    PTABLE_ACCESS_DEFAULT, 0, 1);
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_VNODE_MAP);
    }

    // The VNode meta data
    newvnode->is_vnode  = true;
    newvnode->entry     = entry;
    newvnode->next      = root->u.vnode.children;
    root->u.vnode.children = newvnode;
    newvnode->u.vnode.children = NULL;

    *retvnode = newvnode;
    return SYS_ERR_OK;
}

static void handler(enum exception_type type, int subtype, void *vaddr,
        arch_registers_state_t *regs, arch_registers_fpu_state_t *fpuregs)
{
    DEBUG_COW("got exception %d(%d) on %p\n", type, subtype, vaddr);
    assert(type == EXCEPT_PAGEFAULT);
    assert(subtype == PAGEFLT_WRITE);
    uintptr_t addr = (uintptr_t) vaddr;
    uintptr_t faddr = addr & ~BASE_PAGE_MASK;
    faddr = faddr;
    DEBUG_COW("got expected write pagefault on %p, creating copy of page\n", vaddr);
    struct vnode *ptable = NULL;
    struct capref newframe;
    void *temp;
    struct vregion *tempvr;
    size_t size = 0;
    //TODO: get&use Reto's clone_vnode() for
    //TODO: cow_get_ptable(pmap, faddr, &ptable);
    frame_alloc(&newframe, BASE_PAGE_SIZE, &size);
    // TODO: clone_frame(newframe, newoffset, frame, offset, size)
    vspace_map_one_frame(&temp, BASE_PAGE_SIZE, newframe, NULL, &tempvr);
    memcpy(temp, (void *)faddr, BASE_PAGE_SIZE);
    assert(size == BASE_PAGE_SIZE);
    vregion_destroy(tempvr);
    // TODO: allow overwrite mapping + TLB flush
    vnode_map(ptable->u.vnode.cap, newframe, X86_64_PTABLE_BASE(faddr),
            PTABLE_READ_WRITE, 0, 1);
    USER_PANIC("exhandler NYI");
}

errval_t pmap_cow_init(void)
{
    errval_t err;
    err = thread_set_exception_handler(handler, NULL, ex_stack,
            ex_stack+EX_STACK_SIZE, NULL, NULL);
    assert(err_is_ok(err));
    return SYS_ERR_OK;
}

static struct vnode *find_vnode(struct vnode *root, uint16_t entry)
{
    assert(root != NULL);
    assert(root->is_vnode);
    struct vnode *n;

    for(n = root->u.vnode.children; n != NULL; n = n->next) {
        if (!n->is_vnode) {
            // check whether entry is inside a large region
            uint16_t end = n->entry + n->u.frame.pte_count;
            if (n->entry <= entry && entry < end) {
                //if (n->entry < entry) {
                //    debug_printf("%d \\in [%d, %d]\n", entry, n->entry, end);
                //}
                return n;
            }
        }
        else if(n->entry == entry) {
            // return n if n is a vnode and the indices match
            return n;
        }
    }
    return NULL;
}

errval_t pmap_setup_cow(struct vregion *vregion, void **retbuf)
{
    errval_t err;
    struct pmap *pmap = get_current_pmap();
    genvaddr_t vregion_base = vregion_get_base_addr(vregion);
    size_t vregion_size = vregion_get_size(vregion);

    size_t pml4e = X86_64_PML4_BASE(vregion_base);
    // no support for regions that are not in a single pml4e
    if (pml4e != X86_64_PML4_BASE(vregion_base + vregion_size - 1)) {
        debug_printf("vregion spanning pml4 entries\n");
        return LIB_ERR_NOT_IMPLEMENTED; //XXX
    }

    genvaddr_t new_vaddr;
    err = pmap->f.determine_addr_raw(pmap, vregion_size, 0, &new_vaddr);
    assert(err_is_ok(err));
    size_t new_pml4e = X86_64_PML4_BASE(new_vaddr);
    debug_printf("using pml4e %zu to alias pml4e %zu\n",
            new_pml4e, pml4e);

    struct pmap_x86 *x86 = (struct pmap_x86*)pmap;

    cow_root_pte = find_vnode(&x86->root, pml4e);
    debug_printf("cow_root_pte:%p\n", cow_root_pte);
    assert(cow_root_pte);
    // TODO: better change to r/o on pml4e or pdpt?
    err = vnode_modify_flags(cow_root_pte->u.vnode.cap, 0,
            PTABLE_SIZE, PTABLE_ACCESS_READONLY);
    assert(err_is_ok(err));
    // create copy of pdpt cap
    struct capref copy;
    err = slot_alloc(&copy);
    assert(err_is_ok(err));
    err = cap_copy(copy, cow_root_pte->u.vnode.cap);
    assert(err_is_ok(err));

    struct vnode *copy_vnode = NULL;
    err = alloc_vnode_noalloc(x86, &x86->root, copy,
                      new_pml4e, &copy_vnode);
    assert(copy_vnode);
    // XXX: dangerous!
    copy_vnode->u.vnode.children = cow_root_pte->u.vnode.children;

    *retbuf = (void *)(uintptr_t)(new_pml4e << 39);

    return err;
}
