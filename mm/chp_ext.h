#ifndef _CHP_EXT_H_
#define _CHP_EXT_H_
// prepare for kernel module
#include <linux/mm.h>

#ifdef CONFIG_CONT_PTE_HUGEPAGE
struct chp_ext_order {
	union {
		struct {
			unsigned int order : 8;
			unsigned int magic : 16;
			unsigned int type : 8;
		};
		unsigned int nr;
	};
};

enum chp_ext_type {
	CHP_EXT_ZSMALLOC = 0,
	CHP_EXT_GPU,
	CHP_EXT_DMABUF,

	NR_CHP_EXT_TYPES,
};

static inline struct page *alloc_chp_ext_wrapper(gfp_t gfp_mask, int type)
{
	struct chp_ext_order ceo = {
		.order = HPAGE_CONT_PTE_ORDER,
		.magic = THP_SWAP_PRIO_MAGIC,
		.type = type,
	};

	return alloc_pages(gfp_mask, ceo.nr);
}

static inline void __free_pages_ext(struct page *page, unsigned int order)
{
	if(unlikely(order == HPAGE_CONT_PTE_ORDER && PageContExtAlloc(page)))
		put_page(page);
	else
		__free_pages(page, order);
}

static inline bool is_chp_ext_pages(struct page *page, unsigned int order)
{
	return (order == HPAGE_CONT_PTE_ORDER && PageContExtAlloc(page));
}
#endif
#endif
