#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/list_sort.h>
#include <linux/poll.h>
#include <linux/compat.h>
#include <linux/random.h>

#define TCM_NAME		"tcm"

#define IOC_MAGIC		'c'
#define TCM_MEM_SHOW		_IOR(IOC_MAGIC, 2, int)
#define TCM_VA_TO_PA		_IOR(IOC_MAGIC, 4, int)
#define TCM_REQUEST_MEM		_IOR(IOC_MAGIC, 5, int)
#define TCM_RELEASE_MEM		_IOR(IOC_MAGIC, 6, int)

#define MM_MIN_SHIFT		(PAGE_SHIFT)  /* 16 bytes */
#define MM_MIN_CHUNK		(1 << MM_MIN_SHIFT)
#define MM_GRAN_MASK		(MM_MIN_CHUNK-1)
#define MM_ALIGN_UP(a)		(((a) + MM_GRAN_MASK) & ~MM_GRAN_MASK)
#define MM_ALIGN_DOWN(a)	((a) & ~MM_GRAN_MASK)
#define MM_ALLOC_BIT		(0x80000000)

typedef struct {
	size_t			vaddr;
	size_t			size;
} block_t;

typedef struct {
	size_t			addr_base;
	int			block_num;
	block_t			*block;
	struct device		*dev;
	struct mutex 		mutex;
	wait_queue_head_t 	wait;
	struct list_head	req_head;
} tcm_t;

typedef struct {
	struct list_head 	list;
	uintptr_t		paddr;		/* Phy addr of memory */
	size_t			size;		/* Size of this chunk */
	uintptr_t		next_paddr;
	int			block_id;
	void			*caller;
} mm_node_t;

typedef struct {
	struct list_head	head;
	size_t			max_size;
} list_manager_t;

typedef struct {
	size_t			mm_heapsize;
	size_t 			free_size;
	uintptr_t		start;
	uintptr_t		end;
	list_manager_t		free;
	list_manager_t		alloc;
} mm_heap_t;

typedef struct {
	struct list_head 	list;
	phys_addr_t		paddr;
	size_t			size;
} mm_alloc_node_t;

typedef struct {
	struct list_head 	list;
	int			pid;
	uint32_t		rand_id;
	size_t			req_size;
	int			timeout;
} request_mem_t;

typedef struct {
	void 			*vaddr;
	void 			*paddr;
} va_to_pa_msg_t;

static tcm_t			tcm;
static mm_heap_t		*g_mmheap;
static int			g_block_num;

static void add_node(mm_heap_t *heap, list_manager_t *list, mm_node_t *node, char *tip)
{
	mm_node_t *cur;

	node->next_paddr = node->paddr + node->size;
	if (list_empty(&list->head)) {
		list_add(&node->list, &list->head);
		dev_dbg(tcm.dev, "[%s] add first node:%lx addr:%lx len:%lx\n", tip, (uintptr_t)node, (uintptr_t)node->paddr, node->size);
		return;
	}

	list_for_each_entry(cur, &list->head, list) {
		if ((size_t)cur->paddr > (size_t)node->paddr ) {
			list_add_tail(&node->list, &cur->list);
			dev_dbg(tcm.dev, "[%s] add node:%lx addr:%lx len:%lx\n", tip, (uintptr_t)node, (uintptr_t)node->paddr, node->size);
			return;
		}
	}

	dev_dbg(tcm.dev, "[%s] add tail node:%lx addr:%lx len:%lx\n", tip, (uintptr_t)node, (uintptr_t)node->paddr, node->size);

	list_add_tail(&node->list, &list->head);
}

static void add_free_node(mm_heap_t *heap, mm_node_t *node)
{
	heap->free_size += node->size;
	add_node(heap, &heap->free, node, "free");
}

static void del_free_node(mm_heap_t *heap, mm_node_t *node)
{
	heap->free_size -= node->size;
	list_del(&node->list);
}

static void add_alloc_node(mm_heap_t *heap, mm_node_t *node)
{
	add_node(heap, &heap->alloc, node, "alloc");
}

static void del_alloc_node(mm_heap_t *heap, mm_node_t *node)
{
	list_del(&node->list);
}

static void mm_addregion(mm_heap_t *heap, void *heapstart, size_t heapsize)
{
	mm_node_t 		 *node;
	uintptr_t 		 heapbase;
	uintptr_t 		 heapend;

	heapbase 		 = MM_ALIGN_UP((uintptr_t)heapstart);
	heapend  		 = MM_ALIGN_DOWN((uintptr_t)heapstart + (uintptr_t)heapsize);
	heapsize 		 = heapend - heapbase;

	heap->mm_heapsize	+= heapsize;
	heap->start		 = heapbase;
	heap->end		 = heapend;

	node			 = (mm_node_t *)(kmalloc(sizeof(mm_node_t), GFP_KERNEL));
	node->paddr		 = heapbase;
	node->size		 = heapsize;

	add_free_node(heap, node);
	dev_dbg(tcm.dev, "mm init(start:0x%lx)(len:0x%lx)\n", heapbase, heapsize);
}

static mm_node_t *get_free_max_node(mm_heap_t *heap, size_t size)
{
	mm_node_t *node, *max_node;

	max_node = list_first_entry(&heap->free.head, mm_node_t, list);
	list_for_each_entry(node, &heap->free.head, list) {
		if (node->size >= size) {
			max_node = node;
			break;
		}
		if (node->size >= max_node->size) {
			max_node = node;
		}
	}

	return max_node;
}

static int node_fission(mm_heap_t *heap, mm_node_t *node, size_t size)
{
	size_t remaining = node->size - size;

	dev_dbg(tcm.dev, "remaining size:%lx\n", remaining);
	if (remaining > 0) {
		mm_node_t *remainder = (mm_node_t *)(kmalloc(sizeof(mm_node_t), GFP_KERNEL));
		if (!remainder) {
			return -1;
		}

		remainder->size		= remaining;
		remainder->paddr	= node->paddr + size;
		node->size		= size;

		add_free_node(heap, remainder);
	}

	del_free_node(heap, node);
	add_alloc_node(heap, node);

	return 0;
}

static void *mm_max_mallc(mm_heap_t *heap, size_t size, size_t *valid_size)
{
	mm_node_t *node;

	size = MM_ALIGN_UP(size);

	node = get_free_max_node(heap, size);

	dev_dbg(tcm.dev, "\n%s node:(%lx)(%lx)(%lx)\n", __func__, (uintptr_t)node, (uintptr_t)node->paddr, node->size);

	if (size <= node->size) {
		node_fission(heap, node, size);
		*valid_size = size;
	} else {
		node_fission(heap, node, node->size);
		*valid_size = node->size;
	}

	return (void *)node->paddr;
}

static mm_node_t *get_node_by_ptr(mm_heap_t *heap, void *mem)
{
	mm_node_t *node;

	list_for_each_entry(node, &heap->alloc.head, list) {
		if ((size_t)node->paddr == (size_t)mem) {
			return node;
		}
	}

	return NULL;
}

static void mm_free(mm_heap_t *heap, void *mem)
{
	mm_node_t *cur, *next, *node;
	int gc_flag = 0;

	node = get_node_by_ptr(heap, mem);
	if (!node) return;

	dev_dbg(tcm.dev, "%s  node:(%lx)(%lx)(%lx)\n", __func__, (uintptr_t)node, (uintptr_t)node->paddr, node->size);

	del_alloc_node(heap, node);

	list_for_each_entry_safe(cur, next, &heap->free.head, list) {
		if (cur->next_paddr == (size_t)node->paddr) {
			cur->size	+= node->size;
			gc_flag		|= 1;

			dev_dbg(tcm.dev, "gc prev succful(%lx)(%lx)(%lx)\n", (uintptr_t)cur, (uintptr_t)cur->paddr, cur->size);
			if (!list_is_last(&cur->list, &heap->free.head)) {
				if (cur->next_paddr == (size_t)next->paddr) {
					cur->size	+= next->size;
					gc_flag		|= 2;
					dev_dbg(tcm.dev, "gc 2 next succful(%lx)(%lx)(%lx)\n", (uintptr_t)cur, (uintptr_t)cur->paddr, cur->size);
					list_del(&next->list);
					kfree(next);
				}
			}
			break;
		}

		if (node->next_paddr == (size_t)cur->paddr) {
			cur->paddr	 = node->paddr;
			cur->size	+= node->size;
			gc_flag		|= 2;
			dev_dbg(tcm.dev, "gc next succful(%lx)(%lx)(%lx)\n", (uintptr_t)cur, (uintptr_t)cur->paddr, cur->size);
			break;;
		}
	}

	if (gc_flag == 0) {
		add_free_node(heap, node);
	} else {
		kfree(node);
	}
}

static void mm_show(mm_heap_t *heap)
{
	mm_node_t *node;
	int i = 0;

	printk("%s start\n", __func__);
	list_for_each_entry(node, &heap->free.head, list) {
		printk("mem free node[%d]: %lx paddr: %lx size:0x%lx\n",
			i ++, (uintptr_t)node, (uintptr_t)node->paddr, node->size);
	}

	i = 0;
	list_for_each_entry(node, &heap->alloc.head, list) {
		printk("mem alloc node[%d]: %lx paddr: %lx size:0x%lx\n",
			i ++, (uintptr_t)node, (uintptr_t)node->paddr, node->size);
	}

	printk("%s end\n", __func__);
}

static int get_id(uintptr_t ptr)
{
	int i;
	for (i = 0; i < g_block_num; i++) {
		if (ptr >= g_mmheap[i].start && ptr < g_mmheap[i].end){
			return i;
		}
	}
	return -1;
}

static void tcm_free(void *ptr)
{
	int id = get_id((uintptr_t)ptr);
	if (id < 0) {
		return;
	}
	mm_free(&g_mmheap[id], ptr);
}

static size_t total_free_size(void)
{
	int i;
	size_t total = 0;

	for (i = 0; i < g_block_num; i++) {
		total += g_mmheap[i].free_size;
	}

	return total;
}

static struct list_head *tcm_discontinuous_malloc(size_t size)
{
	struct list_head *head;
	int i, remain;
	size_t total;

	total = total_free_size();
	if (total < size) return NULL;

	head = kmalloc(sizeof(struct list_head), GFP_KERNEL);
	if (!head) return NULL;

	INIT_LIST_HEAD(head);
	remain = size;

	for (i = 0; i < g_block_num; i++) {
		while (g_mmheap[i].free_size) {
			mm_alloc_node_t *alloc = kmalloc(sizeof(mm_alloc_node_t), GFP_KERNEL);
			alloc->paddr = (phys_addr_t)mm_max_mallc(&g_mmheap[i], remain, &alloc->size);
			list_add(&alloc->list, head);
			remain -= alloc->size;
			if (remain <= 0) {
				break;
			}
		}
		if (remain <= 0) {
			break;
		}
	}

	return head;
}

static int mm_init(mm_heap_t *heap, size_t start, size_t end)
{
	memset(heap, 0, sizeof(mm_heap_t));

	INIT_LIST_HEAD(&heap->free.head);
	INIT_LIST_HEAD(&heap->alloc.head);

	mm_addregion(heap, (void *)start, end - start);

	return 0;
}

static void *tcm_match_pa(unsigned long vaddr)
{
	// TODO
	struct vm_area_struct *vma;
	mm_alloc_node_t *node;
	struct list_head *head;

	vma = find_vma(current->mm, vaddr);
	if (!vma) {
		return NULL;
	}

	head = (struct list_head *)vma->vm_private_data;
	list_for_each_entry(node, head, list) {
		return (void *)node->paddr;
	}

	return NULL;
}

static request_mem_t *get_req_mem_node(int pid)
{
	request_mem_t *cur;

	list_for_each_entry(cur, &tcm.req_head, list) {
		if (pid == cur->pid) {
			return cur;
		}
	}

	return NULL;
}

static int del_req_mem_node(request_mem_t *node)
{
	mutex_lock(&tcm.mutex);
	list_add_tail(&node->list, &tcm.req_head);
	mutex_unlock(&tcm.mutex);

	return 0;
}

static int add_req_mem_node(request_mem_t *node)
{
	mutex_lock(&tcm.mutex);
	list_add_tail(&node->list, &tcm.req_head);
	mutex_unlock(&tcm.mutex);

	return 0;
}

static void tcm_vma_close(struct vm_area_struct *vma)
{
	mm_alloc_node_t *cur, *next;
	struct list_head *head = (struct list_head *)vma->vm_private_data;

	list_for_each_entry_safe(cur, next, head, list) {
		tcm_free((void *)cur->paddr);
		list_del(&cur->list);
		kfree(cur);
	}
	kfree(head);
	dev_dbg(tcm.dev, "wake up block thread");
	wake_up_all(&tcm.wait);
}

static const struct vm_operations_struct tcm_vm_ops = {
	.close = tcm_vma_close,
};

static int mmap_compare(void* priv, const struct list_head* a, const struct list_head* b)
{
	mm_alloc_node_t* da = list_entry(a, mm_alloc_node_t, list);
	mm_alloc_node_t* db = list_entry(b, mm_alloc_node_t, list);

	return ((size_t)da->paddr > (size_t)db->paddr) ? 1 : (((size_t)da->paddr < (size_t)db->paddr) ? -1 : 0);
}

static int tcm_mmap(struct file *file, struct vm_area_struct *vma)
{
	size_t size = vma->vm_end - vma->vm_start;
	phys_addr_t offset = (phys_addr_t)vma->vm_pgoff << PAGE_SHIFT;
	struct page* page = NULL;
	unsigned long pfn;
	unsigned long addr;
	struct list_head *head;
	mm_alloc_node_t *node;

	/* Does it even fit in phys_addr_t? */
	if (offset >> PAGE_SHIFT != vma->vm_pgoff)
		return -EINVAL;

	vma->vm_ops = &tcm_vm_ops;

	mutex_lock(&tcm.mutex);
	head = tcm_discontinuous_malloc(size);
	mutex_unlock(&tcm.mutex);

	if (!head) {
		return -EINVAL;
	}

	list_sort(NULL, head, mmap_compare);

	vma->vm_private_data = head;
	addr = vma->vm_start;

	list_for_each_entry(node, head, list) {
		pfn = node->paddr >> PAGE_SHIFT;
		page = phys_to_page(node->paddr);
		if (!page) {
			return -ENXIO;
		}
		if (remap_pfn_range(vma,
				addr,
				pfn,
				node->size,
				vma->vm_page_prot)) {
			return -EAGAIN;
		}
		addr += node->size;
	}

	return 0;
}

static long tcm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	if (cmd == TCM_MEM_SHOW) {
		int i = 0;
		for (; i < g_block_num; i++) {
			printk("mem block id(%d):\n", i);
			mm_show(&g_mmheap[i]);
		}
	}else if (cmd  == TCM_VA_TO_PA) {
		va_to_pa_msg_t msg;

		if(copy_from_user(&msg, (void *)arg, sizeof(va_to_pa_msg_t))) {
			return -EFAULT;
		}

		msg.paddr = tcm_match_pa((unsigned long)msg.vaddr);

		if(copy_to_user((void *)arg, &msg, sizeof(va_to_pa_msg_t))) {
			return -EFAULT;
		}
	} else if (cmd == TCM_REQUEST_MEM) {
		size_t size;
		request_mem_t *node;

		if(copy_from_user(&size, (void *)arg, sizeof(size_t))) {
			return -EFAULT;
		}

		node = kmalloc(sizeof(request_mem_t), GFP_KERNEL);
		if (!node) return -ENOMEM;

		node->pid = task_pid_nr(current);
		node->req_size = size;
		add_req_mem_node(node);
	} else if (cmd == TCM_RELEASE_MEM) {
		size_t size;
		request_mem_t *node;
		if(copy_from_user(&size, (void *)arg, sizeof(size_t))) {
			return -EFAULT;
		}
		node = get_req_mem_node(task_pid_nr(current));
		if (node) del_req_mem_node(node);
	}

	return 0;
}

static unsigned int tcm_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = 0;

	request_mem_t *node = get_req_mem_node(task_pid_nr(current));
	dev_dbg(tcm.dev, "poll get node(%lx)\n", (size_t)node);

	if (node == NULL) {
		mask = EPOLLERR;
	} else {
		poll_wait(file, &tcm.wait, wait);
		if (total_free_size() >= node->req_size) {
			mask = EPOLLIN;
		}
	}

	return mask;
}

static const struct file_operations tcm_fops = {
	.owner		= THIS_MODULE,
	.mmap		= tcm_mmap,
	.unlocked_ioctl	= tcm_ioctl,
	.poll		= tcm_poll,
};

static struct miscdevice tcm_misc_device = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= TCM_NAME,
	.fops		= &tcm_fops,
};

static const struct of_device_id tcm_dt_ids[] = {
	{ .compatible = "spacemit,k1-pro-tcm", .data = NULL },
	{ .compatible = "spacemit,k1-x-tcm", .data = NULL },
	{}
};

static int tcm_probe(struct platform_device *pdev)
{
	struct resource *res;
	int ret, num;
	struct device_node *np, *child;

	tcm.dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(tcm.dev, "found no memory resource\n");
		return -EINVAL;
	}

	np = tcm.dev->of_node;
	num = (np) ? of_get_available_child_count(np) + 1 : 1;

	dev_dbg(tcm.dev, "-----------child num:%d\n", num);
	g_mmheap = kmalloc(sizeof(mm_heap_t)*num, GFP_KERNEL);
	if (!g_mmheap)
		return -ENOMEM;

	for_each_available_child_of_node(np, child) {
		struct resource child_res;

		ret = of_address_to_resource(child, 0, &child_res);
		if (ret < 0) {
			dev_err(tcm.dev,
				"could not get address for node %pOF\n",
				child);
			return -EINVAL;
		}

		if (child_res.start < res->start || child_res.end > res->end) {
			dev_err(tcm.dev,
				"reserved block %pOF outside the tcm area\n",
				child);
			return -EINVAL;
		}

		mm_init(&g_mmheap[g_block_num], child_res.start , child_res.start + resource_size(&child_res));
		g_block_num ++;

	}

	csr_write(0x5db, 1);
	ret = misc_register(&tcm_misc_device);
	if (ret) {
		dev_err(tcm.dev, "failed to register misc device\n");
		return ret;
	}
	mutex_init(&tcm.mutex);
	init_waitqueue_head(&tcm.wait);
	INIT_LIST_HEAD(&tcm.req_head);
	dev_dbg(tcm.dev, "tcm register succfully\n");
	return 0;
}

static int tcm_remove(struct platform_device *pdev)
{
	dev_dbg(tcm.dev, "tcm deregister succfully\n");
	csr_write(0x5db, 0);
	kfree(g_mmheap);
	misc_deregister(&tcm_misc_device);

	return 0;
}

static struct platform_driver tcm_driver = {
	.driver = {
		.name		= TCM_NAME,
		.of_match_table = tcm_dt_ids,
	},
	.probe	= tcm_probe,
	.remove = tcm_remove,
};

static int __init tcm_init(void)
{
	return platform_driver_register(&tcm_driver);
}

module_init(tcm_init);
