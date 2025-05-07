#define _GNU_SOURCE

#include <sched.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/time.h>
/** compile with -std=gnu99 */
#include <stdint.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <time.h>


uint64_t mte_setup = 0;
uint64_t buffer_size = 0;
uint64_t inner_loops = 0;
uint64_t outer_iteration = 0;
uint64_t cpu_pin = 0;

volatile uint64_t rand_val = 0, rand_val2 = 0;
volatile uint64_t total = 0;

#define MEASURE_TIME(code, header_str)				\
do {								\
	struct timespec start, end;				\
	double elapsed_time;					\
	clock_gettime(CLOCK_MONOTONIC, &start);			\
	{							\
		code;						\
	}							\
	clock_gettime(CLOCK_MONOTONIC, &end);			\
	elapsed_time = (end.tv_sec - start.tv_sec) * 1e9	\
			+ (end.tv_nsec - start.tv_nsec);	\
	printf("%s time is %f ns\n", header_str, elapsed_time);	\
} while (0)

// set tag on the memory region
#define set_tag(tagged_addr) do {					\
	asm volatile("stg %0, [%0]" : : "r" (tagged_addr) : "memory");	\
} while (0)

// Insert with a user-defined tag
#define insert_my_tag(ptr, input_tag)	\
	((unsigned char*)(((uint64_t)(ptr) & ((1ULL << 48)-1)) | ((input_tag) << 56)))

// Insert a random tag
#define insert_random_tag(ptr) ({			\
	uint64_t __val;					\
	asm("irg %0, %1" : "=r" (__val) : "r" (ptr));	\
	__val;						\
})

#define ALIGN_16BYTE(ptr) (ptr & ~(0xf))
#define TAG_VAL 2ULL

typedef struct mte_granule {
	uint64_t obj[2];
} mte_granule_t; /* 16 byte size */

/* create a cyclic pointer chain that covers all words
   in a memory section of the given size in a randomized order */
// Inspired by: https://github.com/afborchert/pointer-chasing/blob/master/random-chase.cpp
void create_random_chain(uint64_t* indices, uint64_t len) {
	// shuffle indices
	for (uint64_t i = 0; i < len; ++i) {
		indices[i] = i;
	}
	for (uint64_t i = 0; i < len-1; ++i) {
		uint64_t j = i + rand()%(len - i);
		if (i != j) {
			uint64_t temp = indices[i];
			indices[i] = indices[j];
			indices[j] = temp;
		}
	}

	total = 0;
}

void pin_cpu(size_t core_ID)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(core_ID, &set);
	if (sched_setaffinity(0, sizeof(cpu_set_t), &set) < 0) {
		printf("Unable to Set Affinity\n");
		exit(EXIT_FAILURE);
	}
}

int init_mte(uint64_t setup_mte){
	int mte_en = PR_MTE_TCF_SYNC;
	/*
	 * Use the architecture dependent information about the processor
	 * from getauxval() to check if MTE is available.
	 */
	if (!((getauxval(AT_HWCAP2)) & HWCAP2_MTE)){
		printf("MTE is not supported on this hardware\n");
		return EXIT_FAILURE;
	}else{
		printf("MTE is supported\n");
	}
	/*
	 * Enable MTE with synchronous checking
	 */
	if (setup_mte == 2)
		mte_en = PR_MTE_TCF_ASYNC;
	if (prctl(PR_SET_TAGGED_ADDR_CTRL,
			PR_TAGGED_ADDR_ENABLE | mte_en | (0xfffe << PR_MTE_TAG_SHIFT),
			0, 0, 0)){
		perror("prctl() failed");
		return EXIT_FAILURE;
	}
	return 0;
}

int print_usage()
{
	printf("mte_bm -m <mte setup option> -s <buffer size> -l <inner loop count> -i <outer loop count> -c <pin to cpu #>\n");
	printf("mte setup option: 0 -- buffer is not tagged, 1 -- buffer tagged and async, 2 -- buffer tagged and sync\n");
	printf("inner loop count: # of inside loop\n");
	printf("outer loop count: # of outer loop\n");
	printf("pin to cpu #: cpu number to pin the task to\n");
	return -1;
}

int parse_options(int argc, char *argv[])
{
	int opt, opt_arg;

	if (argc <= 1)
		return print_usage();	

	while ((opt = getopt(argc, argv, "m:s:l:i:c:")) != -1) {
		opt_arg = strtoul(optarg, NULL, 10);
		switch (opt) {
			case 'm':
				mte_setup = opt_arg;
				break;

			case 's':
				buffer_size = opt_arg*1024; /* size in KBs */
				break;

			case 'l':
				inner_loops = opt_arg;
				break;

			case 'i':
				outer_iteration = opt_arg;
				break;

			case 'c':
				cpu_pin = opt_arg;
				break;

			default:
				return print_usage();
		}
	}

	// Optional: remaining non-option arguments
	if (optind < argc) {
		printf("Non-option arguments:\n");
		while (optind < argc)
			printf("  %s\n", argv[optind++]);

		return print_usage();
	}

	return 0;
}

void stack_emulation_tag_stores(uint64_t* indices, mte_granule_t *ptr, uint64_t granule_count,
								int workload_iter)
{
	unsigned char *ptr_tag1 = NULL;
	unsigned char *ptr_tag2 = NULL;
	unsigned char *ptr_tag3 = NULL;
	unsigned char *ptr_tag4 = NULL;
	unsigned char *ptr_tag5 = NULL;
	unsigned char *ptr_tag6 = NULL;
	uint64_t *ptr1 = NULL, *ptr2 = NULL, *ptr3 = NULL;
	uint64_t *ptr4 = NULL, *ptr5 = NULL, *ptr6 = NULL;
	uint64_t tag1 = 0, tag2 = 0, tag3 = 0, tag4 = 0, tag5 = 0, tag6 = 0;
	int obj1_select = 0, obj2_select = 0, obj3_select = 0;
	int obj4_select = 0, obj5_select = 0, obj6_select = 0;
	// first ensure that pointer has tag (using 2 as tag) inserted in it
	//ptr = (uint64_t *) insert_my_tag((unsigned char *) ptr, TAG_VAL);

	for(int j = 0; j < workload_iter; j++){
		create_random_chain(indices, granule_count);
		__clear_cache(ptr, (char *) ptr + (granule_count*sizeof(mte_granule_t)));
		rand_val = (uint64_t) rand();
		rand_val2 = (uint64_t) rand();

		obj1_select = rand()%2;
		obj2_select = (obj1_select+1)%2;
		obj3_select = (obj2_select+1)%2;
		obj4_select = (obj3_select+1)%2;
		obj5_select = (obj4_select+1)%2;
		obj6_select = (obj5_select+1)%2;

		for (uint64_t i = 0; i < granule_count-5; i++) {
			ptr1 = &ptr[indices[i]].obj[obj1_select];
			ptr2 = &ptr[indices[i+1]].obj[obj2_select];
			ptr3 = &ptr[indices[i+2]].obj[obj3_select];
			ptr4 = &ptr[indices[i+3]].obj[obj4_select];
			ptr5 = &ptr[indices[i+4]].obj[obj5_select];
			ptr6 = &ptr[indices[i+5]].obj[obj6_select];

			ptr1 = (uint64_t *) insert_random_tag((unsigned char *) ptr1);
			tag1 = (((uint64_t) ptr1) >> 56) & 0xf;
			//tag2 = tag1 ? tag1++ : 0;
			tag2 = (tag1+1);
			tag3 = (tag1+2);
			tag4 = (tag1+3);
			tag5 = (tag1+4);
			tag6 = (tag1+5);

			ptr2 = (uint64_t *) insert_my_tag((unsigned char *) ptr2, tag2);
			ptr3 = (uint64_t *) insert_my_tag((unsigned char *) ptr3, tag3);
			ptr4 = (uint64_t *) insert_my_tag((unsigned char *) ptr4, tag4);
			ptr5 = (uint64_t *) insert_my_tag((unsigned char *) ptr5, tag5);
			ptr6 = (uint64_t *) insert_my_tag((unsigned char *) ptr6, tag6);

			ptr_tag1 = (unsigned char *)ALIGN_16BYTE((unsigned long long)ptr1);
			ptr_tag2 = (unsigned char *)ALIGN_16BYTE((unsigned long long)ptr2);
			ptr_tag3 = (unsigned char *)ALIGN_16BYTE((unsigned long long)ptr3);
			ptr_tag4 = (unsigned char *)ALIGN_16BYTE((unsigned long long)ptr4);
			ptr_tag5 = (unsigned char *)ALIGN_16BYTE((unsigned long long)ptr5);
			ptr_tag6 = (unsigned char *)ALIGN_16BYTE((unsigned long long)ptr6);

			set_tag(ptr_tag1);
			set_tag(ptr_tag2);
			set_tag(ptr_tag3);
			set_tag(ptr_tag4);
			set_tag(ptr_tag5);
			set_tag(ptr_tag6);

			// Load followed by store on stack object
			*ptr1 += rand_val;
			// Load followed by store on stack object
			*ptr2 += (i + j);
			// Load followed by store on stack object (ptr2 will have STLF)
			*ptr3 += *ptr2;
			// sink into volatile rand_val
			rand_val += *ptr3 + *ptr1;
			*ptr4 += rand_val2;
			*ptr5 += rand_val2 - (i + j);
			*ptr6 += *ptr4 + *ptr5;
			rand_val2 = *ptr3 + *ptr6;
			// final sink into total
			total = rand_val + rand_val2;
		}
	}
}

void mte_test_bm(uint64_t buffer_size, uint64_t outer_loop, uint64_t inner_loop,
				uint64_t mte_setup)
{
	int protection = PROT_READ | PROT_WRITE;
	int mte_protection = protection;

	if (mte_setup != 0)
		mte_protection |= PROT_MTE;

	unsigned char *buffer = NULL;
	unsigned char *indices = NULL;

	uint64_t granule_count = buffer_size/sizeof(mte_granule_t);
	uint64_t indices_size = granule_count * sizeof(uint64_t);

	buffer = mmap(NULL, buffer_size, mte_protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	indices = mmap(NULL, indices_size, protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );

	for (int i = 0; i < outer_loop; i++) {
		//create_random_chain((uint64_t*)indices, granule_count);

		//__clear_cache(buffer, buffer + buffer_size);
		MEASURE_TIME(
			stack_emulation_tag_stores((uint64_t*)indices, (mte_granule_t *)buffer, granule_count, 
										inner_loop);
			, 
			"MTE_tagstore: Emulation of Stack objects protected"
		);

		/* Ensure that mapping goes away from TLBs before next iteration */
		int ret = madvise(buffer, buffer_size, MADV_DONTNEED);
		ret |= madvise(indices, indices_size, MADV_DONTNEED);

		if (ret != 0) {
			perror("madvise failed");
			exit(EXIT_FAILURE);
		}
	}

}

int main(int argc, char *argv[])
{
	int ret_code = 0;

	if ((ret_code = parse_options(argc, argv)))
		return print_usage();

	pin_cpu(cpu_pin);

	if (init_mte(mte_setup)) {
		printf("setting up mte failed\n");
		return -1;
	}

	mte_test_bm(buffer_size, outer_iteration, inner_loops, mte_setup);
	return 0;
}