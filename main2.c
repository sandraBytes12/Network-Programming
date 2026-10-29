#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define PAGE_SIZE 256
#define NUM_PAGES 256
#define MAX_FRAMES 1024
#define DEFAULT_FRAMES 128
#define TLB_SIZE 16

int main(int argc, char *argv[])
{
    const char *input_name =
        (argc >= 2) ? argv[1] : "addresses.txt";

    int num_frames = DEFAULT_FRAMES;

    /* Optional third command-line argument selects frame count. */
    if (argc >= 3) {
        char *end;
        long requested = strtol(argv[2], &end, 10);

        if (argv[2][0] == '\0' || *end != '\0' ||
            requested < 1 || requested > MAX_FRAMES) {
            fprintf(stderr, "Frame count must be between 1 and %d.\n",
                    MAX_FRAMES);
            return 1;
        }

        num_frames = (int)requested;
    }

    FILE *input = fopen(input_name, "r");
    FILE *backing = fopen("BACKING_STORE.bin", "rb");

    if (input == NULL || backing == NULL) {
        perror("Error opening input or backing store");
        if (input != NULL) fclose(input);
        if (backing != NULL) fclose(backing);
        return 1;
    }

    FILE *out1 = fopen("out1.txt", "w");
    FILE *out2 = fopen("out2.txt", "w");
    FILE *out3 = fopen("out3.txt", "w");

    if (out1 == NULL || out2 == NULL || out3 == NULL) {
        perror("Error opening output files");
        if (out1 != NULL) fclose(out1);
        if (out2 != NULL) fclose(out2);
        if (out3 != NULL) fclose(out3);
        fclose(input);
        fclose(backing);
        return 1;
    }

    signed char physical_memory[MAX_FRAMES][PAGE_SIZE];
    int page_table[NUM_PAGES];
    int frame_to_page[MAX_FRAMES];

    int tlb_pages[TLB_SIZE];
    int tlb_frames[TLB_SIZE];

    /* next_free_frame allocates unused frames before replacement begins. */
    int next_free_frame = 0;

    /* next_victim follows FIFO order after all frames have been filled. */
    int next_victim = 0;

    /* FIFO TLB insertion pointer. */
    int next_tlb = 0;

    for (int i = 0; i < NUM_PAGES; i++)
        page_table[i] = -1;

    for (int i = 0; i < MAX_FRAMES; i++)
        frame_to_page[i] = -1;

    for (int i = 0; i < TLB_SIZE; i++) {
        tlb_pages[i] = -1;
        tlb_frames[i] = -1;
    }

    long address_count = 0;
    long page_faults = 0;
    long tlb_hits = 0;

    long long address;
    int status = 0;
    int scan_result;

    while ((scan_result = fscanf(input, "%lld", &address)) == 1) {
        unsigned int logical = (unsigned int)
            ((uint64_t)address & 0xFFFFu);

        int page = (logical >> 8) & 0xFF;
        int offset = logical & 0xFF;
        int frame = -1;

        address_count++;

        /* Check the TLB first. */
        for (int i = 0; i < TLB_SIZE; i++) {
            if (tlb_pages[i] == page) {
                frame = tlb_frames[i];
                tlb_hits++;
                break;
            }
        }

        if (frame == -1) {
            frame = page_table[page];

            if (frame == -1) {
                page_faults++;

                /* Allocate a free frame before evicting any page. */
                if (next_free_frame < num_frames) {
                    frame = next_free_frame++;
                } else {
                    /* FIFO: evict the page loaded earliest. */
                    frame = next_victim;
                    next_victim = (next_victim + 1) % num_frames;

                    int old_page = frame_to_page[frame];

                    if (old_page != -1) {
                        page_table[old_page] = -1;

                        /* Invalidate any TLB mapping for the evicted page. */
                        for (int i = 0; i < TLB_SIZE; i++) {
                            if (tlb_pages[i] == old_page) {
                                tlb_pages[i] = -1;
                                tlb_frames[i] = -1;
                            }
                        }
                    }
                }

                if (fseek(backing, (long)page * PAGE_SIZE,
                          SEEK_SET) != 0 ||
                    fread(physical_memory[frame], 1, PAGE_SIZE,
                          backing) != PAGE_SIZE) {
                    fprintf(stderr, "Error loading page %d.\n", page);
                    status = 1;
                    break;
                }

                page_table[page] = frame;
                frame_to_page[frame] = page;
            }

            /* FIFO TLB: insert at the next replacement position. */
            int existing = -1;

            for (int i = 0; i < TLB_SIZE; i++) {
                if (tlb_pages[i] == page) {
                    existing = i;
                    break;
                }
            }

            if (existing != -1) {
                tlb_frames[existing] = frame;
            } else {
                tlb_pages[next_tlb] = page;
                tlb_frames[next_tlb] = frame;
                next_tlb = (next_tlb + 1) % TLB_SIZE;
            }
        }

        int physical_address = frame * PAGE_SIZE + offset;
        int value = (int)physical_memory[frame][offset];

        if (fprintf(out1, "%u\n", logical) < 0 ||
            fprintf(out2, "%d\n", physical_address) < 0 ||
            fprintf(out3, "%d\n", value) < 0) {
            fprintf(stderr, "Error writing output files.\n");
            status = 1;
            break;
        }
    }

    if (scan_result != EOF && scan_result != 1 && status == 0) {
        fprintf(stderr, "Invalid input: expected numeric addresses.\n");
        status = 1;
    }

    if (ferror(input)) {
        perror("Error reading addresses");
        status = 1;
    }

    fprintf(stderr, "Algorithm: FIFO\n");
    fprintf(stderr, "Frames: %d\n", num_frames);
    fprintf(stderr, "Addresses translated: %ld\n", address_count);
    fprintf(stderr, "Page faults: %ld\n", page_faults);
    fprintf(stderr, "Page fault rate: %.6f\n",
            address_count ? (double)page_faults / address_count : 0.0);
    fprintf(stderr, "TLB hits: %ld\n", tlb_hits);
    fprintf(stderr, "TLB hit rate: %.6f\n",
            address_count ? (double)tlb_hits / address_count : 0.0);

    fclose(input);
    fclose(backing);
    fclose(out1);
    fclose(out2);
    fclose(out3);

    return status;
}
