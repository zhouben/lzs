/*
 * lzs dma module for kernel 2.6.10 
 *
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/uio.h>
#include <asm/uaccess.h>
#include <asm/dma-mapping.h>
#include <asm/scatterlist.h>

#include "async_dma.h"
#include "lzf_chip.h"
#include "lzf_chdev.h"

/* Debug */
#define dprintk(format, a...) \
        do { \
                if (debug) printk("%s:%d "format, __FUNCTION__, __LINE__, ##a);\
        } while (0)

static int debug = 0;
MODULE_PARM(debug, "i");
MODULE_PARM_DESC(debug, "debug flag");

/* backport hexdump.c */
enum {
        DUMP_PREFIX_NONE,
        DUMP_PREFIX_ADDRESS,
        DUMP_PREFIX_OFFSET
};
#define bool int
#define hex_asc(x)  "0123456789abcdef"[x]
#include "hexdump.c"

enum {
        MIN_QUEUE = 256,
        MAX_QUEUE = 512,
};

static kmem_cache_t *_cache, *job_cache;
#define MYNAM "lzf_dma"

typedef struct {
        volatile uint8_t __iomem *address;
        uint32_t value;
} hw_register_t;

struct lzf_device {
        uint32_t bases;
        int base_size;
        struct pci_dev *dev;

        volatile uint8_t __iomem *mmr_base;
        int irq;

        /* registers */
        struct {
                hw_register_t CCR,
                              CSR,
                              NDAR,
                              DAR;
        } R;

        /* queue and lock */
        spinlock_t desc_lock;
        struct list_head used_head, free_head;

        wait_queue_head_t wait;
        atomic_t queue, intr;

        uint8_t cookie;

        atomic_t mem_free;
        struct list_head mem_head;
        spinlock_t mem_lock;
};
static struct lzf_device *first_ioc; /* XXX */

void async_dump_register(void)
{
        int i = 0, j = 0, off = 0;
        for (j = 0; j < 8; j ++) {
                printk("%02x: ", j*4);
                for (i = 0; i < 4; i ++) {
                        printk(" %08X ", readl(first_ioc->mmr_base + off));
                        off += 4;
                }
                printk("\n");
        }
}

typedef struct {
        dma_addr_t addr;
        void *virt;
        struct list_head entry;

        void *orig_virt;
        dma_addr_t orig_addr;
} coherent_t;

static coherent_t *
coherent_alloc(struct lzf_device *ioc) 
{
        coherent_t *p = NULL;

        spin_lock_bh(&ioc->mem_lock);
        if (!list_empty(&ioc->mem_head)) {
                p = container_of(ioc->mem_head.next, coherent_t, entry);
                list_del(&p->entry);
        }
        spin_unlock_bh(&ioc->mem_lock);
        atomic_dec(&ioc->mem_free);
        dprintk("p %p, %d\n", p, atomic_read(&ioc->mem_free));

        return p;
}

static void 
coherent_free(struct lzf_device *ioc, coherent_t *p)
{
        dprintk("p %p, %d\n", p, atomic_read(&ioc->mem_free));
        spin_lock_bh(&ioc->mem_lock);
        list_add_tail(&p->entry, &ioc->mem_head);
        spin_unlock_bh(&ioc->mem_lock);
        atomic_inc(&ioc->mem_free);
}

static int init_coherent_one(struct lzf_device *ioc)
{
        coherent_t *o = NULL;
        void *virt, *end, *p;
        dma_addr_t addr, q;
        int len = PAGE_SIZE;

        p = virt = dma_alloc_coherent(&ioc->dev->dev, len, &addr, GFP_KERNEL);
        dprintk("virt %p, %08x\n", virt, addr);
        q = addr;
        end  = virt + len;

        while (p < end) {
                o = kmem_cache_alloc(_cache, GFP_KERNEL);
                /*dprintk("o %p, virt %p, %08x\n", o, p, q);*/
                o->addr = q;
                o->virt = p;
                list_add_tail(&o->entry, &ioc->mem_head);
                o->orig_addr = 0;
                o->orig_virt = NULL;

                q += 32;
                p += 32;
                atomic_inc(&ioc->mem_free);
        }

        o->orig_addr = addr;
        o->orig_virt = virt;
}

static void free_coherent_pool(struct lzf_device *ioc)
{
        coherent_t *p, *q;

        list_for_each_entry_safe(p, q, &ioc->mem_head, entry) {
                /*dprintk("p %p, virt %p, %08x\n", 
                                p, p->virt, p->addr);*/
                if (p->orig_virt) {
                        dprintk("p %p, virt %p, %08x\n", 
                                        p, p->orig_virt, p->orig_addr);
                        dma_free_coherent(&ioc->dev->dev, PAGE_SIZE, 
                                        p->orig_virt, p->orig_addr);
                }
                list_del(&p->entry);
                kmem_cache_free(_cache, p);
        }
}

typedef struct {
        job_desc_t *desc;
        dma_addr_t addr;

        int cookie;
        struct list_head entry, job_entry;

        buf_desc_t *src, *dst;
        res_desc_t *res;

        void *priv;
        async_cb_t cb;

        sgbuf_t *src_buf, *dst_buf;
        int s_cnt, d_cnt;

        coherent_t *q1, *q2;
} job_entry_t;

static job_entry_t *new_job_entry(struct lzf_device *ioc)
{
        job_entry_t *p;
        coherent_t *q;

        p = kmem_cache_alloc(job_cache, GFP_KERNEL);
        if (p == NULL)
                return NULL;
        memset(p, 0, sizeof(*p));

        q = coherent_alloc(ioc);
        p->desc = q->virt;
        p->addr = q->addr;
        p->q1   = q;
        BUG_ON(p->addr & 0x7);

        q = coherent_alloc(ioc);
        p->res = q->virt;
        p->desc->ctl_addr = q->addr;
        p->q2   = q;
        BUG_ON(p->desc->ctl_addr & 0x7);

        return p;
}

static job_entry_t *get_job_entry(struct lzf_device *ioc)
{
        job_entry_t *p = NULL;
        
        BUG_ON(in_irq());
        dprintk(MYNAM ": queue %x\n", atomic_read(&ioc->queue));
        wait_event(ioc->wait, atomic_read(&ioc->queue) < MIN_QUEUE);

        spin_lock_bh(&ioc->desc_lock);
        if (!list_empty(&ioc->free_head)) {
                p = container_of(ioc->free_head.next, job_entry_t, entry);
                list_del(&p->entry);
        }
        spin_unlock_bh(&ioc->desc_lock);
        atomic_inc(&ioc->queue);

        p->src = NULL;
        p->dst = NULL;
        p->src_buf = NULL;
        p->dst_buf = NULL;
        p->desc->next_desc = 0;
        p->desc->dc_fc = 0;
        p->desc->src_desc = 0;
        p->desc->dst_desc = 0;
        p->s_cnt = 0;
        p->d_cnt = 0;

        return p;
}

static int unmap_bufs(struct lzf_device *ioc, buf_desc_t *d, int dir,
                sgbuf_t *s, int cnt, char *pre)
{
        int res = 0;

        while (d) {
                buf_desc_t *n;
                coherent_t *q   = (coherent_t *)d->u[2];
                dma_addr_t addr = d->u[0];
                n = (buf_desc_t *)d->u[1];
                /* free it */
                addr = d->desc_adr;
                dprintk("b %p, desc_next %08x, desc %08x, adr %08x, hw %08x\n",
                                d, d->desc_next, d->desc, d->desc_adr,
                                addr);
                coherent_free(ioc, q);
                cnt --;
                d = n;
        }
        /* unmap data buffer */
        if (s->use_sg == 0) 
                pci_unmap_single(ioc->dev, s->addr, s->bufflen, dir);
        else {
                pci_unmap_sg(ioc->dev, (struct scatterlist *)s->buffer,
                                s->use_sg, dir);
        }

        if (debug && s->use_sg) {
                struct scatterlist *sg = (struct scatterlist *)s->buffer;
                int i;
                for (i = 0; i < s->use_sg && debug; i++, sg++) {
                        char *virt = page_address(sg->page) + sg->offset;
                        print_hex_dump_bytes(pre, DUMP_PREFIX_ADDRESS, virt, 
                                        sg->length);
                }
        } else if (debug) {
                print_hex_dump_bytes(pre, DUMP_PREFIX_ADDRESS, s->buffer, 
                                s->bufflen);
        }

        /* safe check */
        if (cnt != 0) {
                printk("lzf_dma: cnt %d is not zero\n", cnt);
        }

        return res;
}

static buf_desc_t *map_bufs(struct lzf_device *ioc, sgbuf_t *s, int dir, 
                int *c)
{
        int bytes_to_go = s->bufflen;
        buf_desc_t *b = NULL, *prev = NULL, *h = NULL;
        dma_addr_t addr = 0;
        struct scatterlist *sgl = NULL;
        dma_addr_t hw_addr = 0;

        if (s->use_sg == 0) {
                dprintk("bytes_to_go %x, %x\n", 
                                bytes_to_go, LZF_MAX_SG_ELEM_LEN);
                s->addr = addr = 
                        pci_map_single(ioc->dev, s->buffer, s->bufflen, dir);
                if (bytes_to_go <= LZF_MAX_SG_ELEM_LEN) {
                        coherent_t *q = coherent_alloc(ioc);
                        b = q->virt;
                        hw_addr = q->addr;
                        BUG_ON(hw_addr & 0x7);
                        (*c) ++;
                        b->desc_next = 0;
                        b->desc = bytes_to_go;
                        b->desc|= LZF_SG_LAST;
                        b->desc_adr = addr;
                        BUG_ON(b->desc_adr & 0x7);
                        dprintk("b %p, desc_next %08x, desc %08x, adr %08x, hw %08x\n",
                                        b, b->desc_next, b->desc, b->desc_adr,
                                        hw_addr);
                        /* sf */ 
                        b->u[0] = hw_addr;
                        b->u[1] = 0;
                        b->u[2] = (uint32_t)q;
                        return b;
                }
        } else {
                sgl = (struct scatterlist *)s->buffer;
                pci_map_sg(ioc->dev, sgl, s->use_sg, dir);
        }

        dprintk("bytes_to_go %x, %x, %x\n", bytes_to_go, LZF_MAX_SG_ELEM_LEN, 
                        s->use_sg);
        while (bytes_to_go > 0) {
                int this_mapping_len = sgl ? sg_dma_len(sgl) : bytes_to_go;
                int offset = 0;
                dprintk("this_mapping_len %x\n", this_mapping_len);
                while (this_mapping_len > 0) {
                        coherent_t *q = coherent_alloc(ioc);
                        int this_len = min_t(int, LZF_MAX_SG_ELEM_LEN, 
                                        this_mapping_len);
                        dprintk("this_len %x\n", this_len);
                        b = q->virt;
                        hw_addr = q->addr;
                        BUG_ON(hw_addr & 0x7);
                        (*c) ++;
                        b->desc_next = 0; /* will fix later */
                        b->desc = this_len;
                        b->desc_adr = (sgl?sg_dma_address(sgl):addr) + offset;
                        BUG_ON(b->desc_adr & 0x7);
                        dprintk("b %p, desc_next %08x, desc %08x, adr %08x, hw %08x\n",
                                        b, b->desc_next, b->desc, b->desc_adr,
                                        hw_addr);
                        /* sf */
                        b->u[0] = hw_addr;
                        b->u[2] = (uint32_t)q;
                        if (prev == NULL) {
                                h = b;
                        } else {
                                prev->u[1] = (uint32_t)b;
                                prev->desc_next = hw_addr;
                        }
                        prev = b;

                        /* adjust len */
                        this_mapping_len -= this_len;
                        bytes_to_go -= this_len;
                        offset += this_len;
                }
                if (sgl)
                        sgl++;
        }
        prev->desc |= LZF_SG_LAST;
        prev->u[1] = 0;

        return h;
}

static int dc_ay[] = {
        [OP_NULL]       = DC_NULL,
        [OP_FILL]       = DC_FILL,
        [OP_MEMCPY]     = DC_MEMCPY,
        [OP_COMPRESS]   = DC_COMPRESS,
        [DC_UNCOMPRESS] = OP_UNCOMPRESS, 
};

int async_submit(sgbuf_t *src, sgbuf_t *dst, async_cb_t cb, int ops, 
                void *p, int commit)
{
        int res = 0;
        job_entry_t *d, *prev;
        LIST_HEAD(new_chain);
        struct lzf_device *ioc = first_ioc; /* TODO */

        d = get_job_entry(ioc);
        if (src) {
                d->src_buf = src;
                d->src = map_bufs(ioc, src, PCI_DMA_TODEVICE, &d->s_cnt);
        }
        if (dst) {
                d->dst_buf = dst;
                d->dst = map_bufs(ioc, dst, PCI_DMA_FROMDEVICE, &d->d_cnt);
        }

        /* fill the hw desc */
        d->desc->next_desc = 0;
        d->desc->dc_fc  = dc_ay[ops] | DC_INTR_EN /*| DC_CTRL*/;
        d->desc->src_desc = d->src->u[0];
        d->desc->dst_desc = d->dst->u[0];
        dprintk("job hw addr %08x, dc_fc %08x, src %08x, dst %08x, %p\n", 
                        d->addr, d->desc->dc_fc, d->desc->src_desc, 
                        d->desc->dst_desc, d);

        /* callback function */
        d->cb = cb;
        d->priv = p;
        d->cookie = ioc->cookie | 1<<31;
        d->cookie ++;

        spin_lock_bh(&ioc->desc_lock);
        prev = container_of(ioc->used_head.prev, job_entry_t, entry);
        dprintk("last desc %p\n", prev);
        prev->desc->next_desc = d->addr;
        prev->desc->dc_fc |= DC_CONT;

        list_add(&d->entry, &new_chain);
        __list_splice(&new_chain, ioc->used_head.prev);

        if (commit)
                writel(CCR_APPEND|CCR_ENABLE, ioc->R.CCR.address);
        spin_unlock_bh(&ioc->desc_lock);

        return res;
}
EXPORT_SYMBOL(async_submit);
EXPORT_SYMBOL(async_dump_register);

/* 
 * Psuedo code:
 *  pci_unmap all buffers, include src, dst, result
 *  doing callback.
 *  freeing the resource.
 */
static int do_job_one(struct lzf_device *ioc, job_entry_t *d)
{
        int res = 0;

        /* unmap result data */
        if (d->src)
                unmap_bufs(ioc, d->src, PCI_DMA_TODEVICE, d->src_buf, 
                                d->s_cnt, "src ");
        if (d->dst)
                unmap_bufs(ioc, d->dst, PCI_DMA_FROMDEVICE, d->dst_buf, 
                                d->d_cnt, "dst ");

        dprintk("cb %p,%p, err %x, ocnt %x\n", d->cb, d->priv, d->res->err, 
                        d->res->ocnt);

        d->cb(d->priv, d->res->err, d->res->ocnt);

        atomic_dec(&ioc->queue);
        /*wake_up_all(&ioc->wait);*/

        return res;
}

static int do_jobs(struct lzf_device *ioc)
{
        job_entry_t *d, *t;
        uint32_t phys_complete;
        LIST_HEAD(head);
        int res = 0;

        /*if (!spin_trylock_bh(&ioc->desc_lock))
                return;*/
        phys_complete = readl(ioc->R.DAR.address);
        dprintk("phys %x\n", phys_complete);
        list_for_each_entry_safe(d, t, &ioc->used_head, entry) {
                dprintk("addr %x, cookie %x\n", d->addr, d->cookie);
                if (d->cookie)
                        list_add_tail(&d->job_entry, &head);
                if (d->addr != phys_complete) {
                        list_del(&d->entry);
                        list_add_tail(&d->entry, &ioc->free_head);
                } else {
                        d->cookie = 0;
                        break;
                }
        }
        /*spin_unlock_bh(&ioc->desc_lock);*/

        list_for_each_entry_safe(d, t, &head, job_entry) {
                dprintk("addr %x, cookie %x\n", d->addr, d->cookie);
                do_job_one(ioc, d);
                list_del(&d->job_entry);
        }
        wake_up_all(&ioc->wait);

        return res;
}

static int lzf_intr_handler(int irq, void *p, struct pt_regs *regs)
{
        uint32_t val = 0;
        int res = IRQ_NONE;
        struct lzf_device *ioc = p;

        val = readl(ioc->R.CSR.address);
        if ((val & CSR_INTP) == 0) { /* interrupt pending */
                goto out;
        }
        res = IRQ_HANDLED;

        /* clear irq flags */
        val = readl(ioc->R.CCR.address);
        val |= CCR_C_INTP;
        writel(val, ioc->R.CCR.address);
        wmb();
        val = readl(ioc->R.CCR.address);

        atomic_inc(&ioc->intr);
        /* call the finished jobs */
        do_jobs(ioc);

out:
        return res;
}

static void start_null_desc(struct lzf_device *ioc)
{
        job_entry_t *d;
        uint32_t next_desc, ctl_addr;

        /*val = readl(ioc->mmr_base + 0x7C);
        printk("MAGIC: %08X\n", val);
        BUG_ON(val != 0xAA55);*/

        /* reset device */
        writel(0, ioc->R.CCR.address);

        d = get_job_entry(ioc);
        d->desc->next_desc = 0x300;
        d->desc->ctl_addr  = 0x200;
        d->desc->dc_fc     = DC_NULL|DC_INTR_EN;
        dprintk("job hw addr %08x, dc_fc %08x, src %08x, dst %08x, %p\n", 
                        d->addr, d->desc->dc_fc, d->desc->src_desc, 
                        d->desc->dst_desc, d);

        spin_lock_bh(&ioc->desc_lock);
        d->cookie = 0;
        list_add_tail(&d->entry, &ioc->used_head);
        spin_unlock_bh(&ioc->desc_lock);

        writel(d->addr, ioc->R.NDAR.address);
        writel(CCR_ENABLE, ioc->R.CCR.address);
       
        /* data path check */
        wait_event(ioc->wait, atomic_read(&ioc->intr));
        next_desc = readl(ioc->mmr_base + 0x14*4);
        ctl_addr  = readl(ioc->mmr_base + 0x16*4);
        /* next_desc  must = 0x200
         * ctl_addr   must = 0x300
         */
        dprintk("next desc %08x, ctrl_addr %08x\n", next_desc, ctl_addr);
}

static int __devinit lzf_probe(struct pci_dev *pdev, 
                const struct pci_device_id *id)
{
        struct lzf_device *ioc;
        int res = -ENODEV, i = 0;
        uint32_t val;
        struct resource *r;

        if (pci_enable_device(pdev))
                return res;
        ioc = kmalloc(sizeof(*ioc), GFP_KERNEL);
        ioc->bases = pci_resource_start(pdev, 1);
        ioc->base_size = pci_resource_len(pdev, 1);
        ioc->dev = pdev;
        ioc->irq = pdev->irq;

        /* enable mmio and master */
        pci_read_config_dword(pdev, PCI_COMMAND, &val);
        val |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
        pci_write_config_dword(pdev, PCI_COMMAND, val);
        /* Set burst length */
        pci_write_config_dword(pdev, PCI_CACHE_LINE_SIZE, 0x4004);
        val = 1<<0; /* MRL */
        val |=1<<1; /* prefetch */
        pci_write_config_dword(pdev, 0x184, val);
        /* base address */
        pci_write_config_dword(pdev, 0x188, 0x0);
        /* address mask */
        pci_write_config_dword(pdev, 0x18c, 0x80000000);
        /* intr enable */
        pci_write_config_dword(pdev, 0x1ec, 0x1);

        r = request_mem_region(ioc->bases, ioc->base_size, MYNAM);
        if (!r) {
                printk(KERN_ERR MYNAM ": ERROR - reserved base 1 failed\n");
                kfree(ioc);
                return res;
        }
        ioc->mmr_base = ioremap(ioc->bases, ioc->base_size);
        ioc->R.CCR.address  = ioc->mmr_base + OFS_CCR;
        ioc->R.CSR.address  = ioc->mmr_base + OFS_CSR;
        ioc->R.DAR.address  = ioc->mmr_base + OFS_DAR;
        ioc->R.NDAR.address = ioc->mmr_base + OFS_NDAR;

        res = request_irq(pdev->irq, lzf_intr_handler, SA_SHIRQ, MYNAM, ioc);
        if (res) {
                printk(KERN_ERR MYNAM ": ERROR - reserved irq %d failed\n",
                                ioc->irq);
                kfree(ioc);
                return res;
        }
        pci_set_drvdata(pdev, ioc);
        INIT_LIST_HEAD(&ioc->free_head);
        INIT_LIST_HEAD(&ioc->used_head);
       
        atomic_set(&ioc->mem_free, 0);
        INIT_LIST_HEAD(&ioc->mem_head);
        spin_lock_init(&ioc->mem_lock);
        for (i = 0; i < 256; i++)
                init_coherent_one(ioc);

        for (i = 0; i < MAX_QUEUE; i++) {
                job_entry_t *j;
                j = new_job_entry(ioc);
                list_add_tail(&j->entry, &ioc->free_head);
        }
        init_waitqueue_head(&ioc->wait);
        spin_lock_init(&ioc->desc_lock);
        atomic_set(&ioc->queue, 0);
        atomic_set(&ioc->intr, 0);

        start_null_desc(ioc);
        first_ioc = ioc;
        ioc->cookie = 0;

        return res;
}

static void __devexit lzf_remove(struct pci_dev *pdev)
{
        struct lzf_device *ioc = pci_get_drvdata(pdev);
        job_entry_t *j, *t;
        LIST_HEAD(head);

        list_for_each_entry_safe(j, t, &ioc->free_head, entry) {
                list_del(&j->entry);
                list_add_tail(&j->entry, &head);
        }
        list_for_each_entry_safe(j, t, &ioc->used_head, entry) {
                list_del(&j->entry);
                list_add_tail(&j->entry, &head);
        }
        list_for_each_entry_safe(j, t, &head, entry) {
                list_del(&j->entry);
                coherent_free(ioc, j->q1);
                coherent_free(ioc, j->q2);
                kmem_cache_free(job_cache, j);
        }

        free_irq(ioc->irq, ioc);
        iounmap((void __iomem *)ioc->mmr_base);
        release_mem_region(ioc->bases, ioc->base_size);
        free_coherent_pool(ioc);

        kfree(ioc);
        pci_set_drvdata(pdev, NULL);
}

static void lzf_shutdown(struct device *dev)
{
        /* TODO */
}

static struct pci_device_id lzf_pci_table[] = {
        { 0x0100, 0x0003, PCI_ANY_ID, PCI_ANY_ID},
        { 0 },
};

static struct pci_driver lzf_driver = {
        .name      = "lzf",
        .id_table  = lzf_pci_table,
        .probe     = lzf_probe,
        .remove    = __devexit_p(lzf_remove),
        .driver    = {
                .shutdown = lzf_shutdown,
        },
};


static int __init lzf_init(void)
{
        init_chdev();

        _cache = kmem_cache_create("coherent_t", 
                        sizeof(coherent_t),
                        0, 0, NULL, NULL);
        job_cache = kmem_cache_create("job_cache",
                        sizeof(job_entry_t),
                        0, 0, NULL, NULL);
        dprintk("%d, %d, %d\n", 
                        sizeof(job_desc_t),
                        sizeof(res_desc_t),
                        sizeof(buf_desc_t));
        BUG_ON(sizeof(job_desc_t) != 32);
        BUG_ON(sizeof(res_desc_t) != 32);
        BUG_ON(sizeof(buf_desc_t) != 32);
        return pci_module_init(&lzf_driver);
}

static void __exit lzf_exit(void)
{
        exit_chdev();
        pci_unregister_driver(&lzf_driver);
        kmem_cache_destroy(_cache);
        kmem_cache_destroy(job_cache);
}

module_init(lzf_init);
module_exit(lzf_exit);
MODULE_LICENSE("GPL");
