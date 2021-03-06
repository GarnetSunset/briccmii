#include "lib/printk.h"
#include "lib/heap.h"
#include "display/video_fb.h"

#include "hwinit/btn.h"
#include "hwinit/hwinit.h"
#include "hwinit/di.h"
#include "hwinit/mc.h"
#include "hwinit/t210.h"
#include "hwinit/sdmmc.h"
#include "hwinit/timer.h"
#include "hwinit/util.h"
#include "hwinit/fuse.h"
#include "hwinit/se.h"
#include <string.h>
#define XVERSION 2

#define SECTOR_SIZE (512)
#define NUM_BCT_ENTRIES (64)
#define BCT_ENTRY_SIZE_BYTES (0x4000)
#define BCT_ENTRY_SIZE_SECTORS (BCT_ENTRY_SIZE_BYTES/SECTOR_SIZE)
#define NUM_KEYBLOBS (32)
#define KEYBLOB_SIZE (0xB0)

int main(void) 
{
    u32* lfb_base;
    uint32_t btn = 0;

    config_hw();
    display_enable_backlight(0);
    display_init();

    // Set up the display, and register it as a printk provider.
    lfb_base = display_init_framebuffer();
    video_init(lfb_base);

    //Tegra/Horizon configuration goes to 0x80000000+, package2 goes to 0xA9800000, we place our heap in between.
	heap_init(0x90020000);

    printk("                                 briccmii v%d by rajkosto\n", XVERSION);
    printk("\n atmosphere base by team reswitched, hwinit by naehrwert, some parts taken from coreboot\n\n");

    /* Turn on the backlight after initializing the lfb */
    /* to avoid flickering. */
    display_enable_backlight(1);

    unsigned char* bctAlloc = malloc(BCT_ENTRY_SIZE_BYTES * NUM_BCT_ENTRIES + SECTOR_SIZE);
    unsigned char* bctSectors = (void*)ALIGN_UP((uintptr_t)bctAlloc, SECTOR_SIZE);
    memset(bctSectors, 0, BCT_ENTRY_SIZE_BYTES * NUM_BCT_ENTRIES);

    unsigned char* kblobAlloc = malloc(SECTOR_SIZE * NUM_KEYBLOBS + SECTOR_SIZE);
    unsigned char* kblobSectors = (void*)ALIGN_UP((uintptr_t)kblobAlloc, SECTOR_SIZE);
    memset(kblobSectors, 0, SECTOR_SIZE * NUM_KEYBLOBS);

    mc_enable_ahb_redirect();    
    sdmmc_storage_t storage;
    memset(&storage, 0, sizeof(storage));
	sdmmc_t sdmmc;
    memset(&sdmmc, 0, sizeof(sdmmc));

	if (!sdmmc_storage_init_mmc(&storage, &sdmmc, SDMMC_4, SDMMC_BUS_WIDTH_8, 4))
	{
        memset(&storage, 0, sizeof(storage));
		printk("Failed to init eMMC.\n");
		goto progend;
	}

    if (!sdmmc_storage_set_mmc_partition(&storage, 1)) 
    {
        printk("Failed to switch to BOOT0 eMMC partition.\n");
		goto progend;
    }

    if (!sdmmc_storage_read(&storage, 0x180000/SECTOR_SIZE, NUM_KEYBLOBS, kblobSectors))
    {
        printk("Error reading keyblob data from BOOT0.\n");
        goto progend;
    }

    for (u32 currEntry=0; currEntry<NUM_BCT_ENTRIES; currEntry++)
    {
        if (!sdmmc_storage_read(&storage, currEntry*BCT_ENTRY_SIZE_SECTORS, BCT_ENTRY_SIZE_SECTORS, &bctSectors[currEntry*BCT_ENTRY_SIZE_BYTES]))
        {
            printk("Error reading BCT entry %u, exiting.\n", currEntry);
            goto progend;
        }
    }

    uint64_t usedBctEntries = 0;
    for (u32 currEntry=0; currEntry<NUM_BCT_ENTRIES; currEntry++)
    {
        const unsigned char* bctSigData = &bctSectors[currEntry*BCT_ENTRY_SIZE_BYTES + 0x210];
        for (u32 currByte=0; currByte<0x210; currByte++)
        {
            if (bctSigData[currByte] != 0)
            {
                usedBctEntries |= (uint64_t)(1u) << currEntry;
                break;
            }
        }
    }

    uint64_t validBctEntries = 0;
    uint64_t validBctCustData = 0;

    ALIGNED(32) unsigned char correctPubkeyHash[32];
    ALIGNED(32) unsigned char currentHash[32];
    memcpy(correctPubkeyHash, (void*)get_fuse_chip_regs()->FUSE_PUBLIC_KEY, sizeof(correctPubkeyHash));

    uint32_t currFuseCount = 0;
    uint32_t currFuseBits = fuse_get_reserved_odm(7);
    for (u32 currBit=0; currBit<32; currBit++)
    {
        if (currFuseBits & (1u << currBit))
            currFuseCount++;
        else
            break;
    }
    printk("NUMBER OF BURNT ANTI-DOWNGRADE FUSES: %u (raw value: 0x%08X)\n", currFuseCount, currFuseBits);
    printk("Required BCT PUBKEY SHA256:\n");
    for (u32 i=0; i<sizeof(correctPubkeyHash); i++) printk("%02X", (u32)correctPubkeyHash[i]);
    printk("\n\n");

    for (u32 currEntry=0; currEntry<NUM_BCT_ENTRIES; currEntry++)
    {
        const u64 wantedMask = (uint64_t)(1u) << currEntry;
        if ((usedBctEntries & wantedMask) == 0)
            continue;

        printk("BCT entry %u: ", currEntry);

        const unsigned char* bctEntryData = &bctSectors[currEntry*BCT_ENTRY_SIZE_BYTES];
        memset(currentHash, 0, sizeof(currentHash));
        se_calculate_sha256(currentHash, &bctEntryData[0x210], 0x100);
        bool pubkeyCorrect = memcmp(currentHash, correctPubkeyHash, sizeof(correctPubkeyHash)) == 0;
        bool custDataCorrect = false;
        {
            uint32_t bctVersion = 0;
            memcpy(&bctVersion, &bctEntryData[0x2330], sizeof(bctVersion));
            if (bctVersion > 0 && bctVersion <= NUM_KEYBLOBS)
            {
                const unsigned char* wantedKeyblob = &kblobSectors[(bctVersion-1)*SECTOR_SIZE];
                custDataCorrect = memcmp(&bctEntryData[0x450], wantedKeyblob, KEYBLOB_SIZE) == 0;
            }
        }

        if (pubkeyCorrect)
        {
            validBctEntries |= wantedMask;
            printk("PUBKEY CORRECT (NORMAL)!");
        }
        else
            printk("PUBKEY INCORRECT (BRICC'D)!");

        printk(" ");
        if (custDataCorrect)
        {
            validBctCustData |= wantedMask;
            printk("cust_data correct");
        }
        else
            printk("cust_data incorrect");

        printk("\n");
    }

    printk("\nPRESS VOL- TO BRICC ALL OR VOL+ TO TRY AND UNBRICC ALL BCT ENTRIES! (Power button to quit)\n");
    for (;;) 
    {
        btn = btn_read();
        if ((btn & BTN_POWER) != 0)
            goto progend;

        if (btn == BTN_VOL_UP)
        {
            ALIGNED(32) unsigned char goodPubkey[0x100];
            memset(goodPubkey, 0, sizeof(goodPubkey));

            printk("Finding correct pubkey...");
            if (validBctEntries != 0)
            {
                for (u32 currEntry=0; currEntry<NUM_BCT_ENTRIES; currEntry++)
                {
                    const u64 wantedMask = (uint64_t)(1u) << currEntry;
                    if ((validBctEntries & wantedMask) == 0)
                        continue;

                    const unsigned char* bctEntryData = &bctSectors[currEntry*BCT_ENTRY_SIZE_BYTES];
                    memcpy(goodPubkey, &bctEntryData[0x210], sizeof(goodPubkey));
                    printk("Using correct one from BCT entry %u\n", currEntry);
                    break;
                }
            }
            else
            {
                for (u32 currEntry=0; currEntry<NUM_BCT_ENTRIES; currEntry++)
                {
                    const u64 wantedMask = (uint64_t)(1u) << currEntry;
                    if ((usedBctEntries & wantedMask) == 0)
                        continue;

                    const unsigned char* bctEntryData = &bctSectors[currEntry*BCT_ENTRY_SIZE_BYTES];
                    memcpy(goodPubkey, &bctEntryData[0x210], sizeof(goodPubkey));

                    bool foundGoodKey = false;
                    memset(currentHash, 0, sizeof(currentHash));                    
                    for (u32 i=0; i<=0xFF; i++)
                    {
                        goodPubkey[0] = i & 0xFF;
                        se_calculate_sha256(currentHash, goodPubkey, sizeof(goodPubkey));
                        if (memcmp(currentHash, correctPubkeyHash, sizeof(correctPubkeyHash)) == 0)
                        {
                            foundGoodKey = true;
                            break;
                        }
                    }

                    if (foundGoodKey)
                    {
                        printk("Repaired the one in BCT entry %u\n", currEntry);
                        break;
                    }
                }
            }

            memset(currentHash, 0, sizeof(currentHash));
            se_calculate_sha256(currentHash, goodPubkey, sizeof(goodPubkey));

            if (memcmp(currentHash, correctPubkeyHash, sizeof(correctPubkeyHash)) != 0)
            {
                printk("Unable to find correct pubkey!\n");
                break;
            }

            for (u32 currEntry=0; currEntry<NUM_BCT_ENTRIES; currEntry++)
            {
                const u64 wantedMask = (uint64_t)(1u) << currEntry;
                if ((usedBctEntries & wantedMask) == 0)
                    continue;

                printk("BCT entry %u: ", currEntry);
                bool pubkeyCorrect = (validBctEntries & wantedMask) != 0;
                bool custDataCorrect = (validBctCustData & wantedMask) != 0;
                if (pubkeyCorrect && custDataCorrect)
                {
                    printk("ALREADY CORRECT!\n");
                    continue;
                }

                unsigned char* bctEntryData = &bctSectors[currEntry*BCT_ENTRY_SIZE_BYTES];
                if (!custDataCorrect)
                {
                    uint32_t bctVersion = 0;
                    memcpy(&bctVersion, &bctEntryData[0x2330], sizeof(bctVersion));
                    if (bctVersion > 0 && bctVersion <= NUM_KEYBLOBS)
                    {
                        const unsigned char* wantedKeyblob = &kblobSectors[(bctVersion-1)*SECTOR_SIZE];
                        memcpy(&bctEntryData[0x450], wantedKeyblob, KEYBLOB_SIZE);
                        if (!sdmmc_storage_write(&storage, currEntry*BCT_ENTRY_SIZE_SECTORS+2, 1, &bctEntryData[SECTOR_SIZE*2]))
                        {
                            printk("Error writing cust_data to BOOT0!\n");
                            continue;
                        }
                        else
                        {
                            printk("cust_data corrected\n");
                            continue;
                        }
                    }
                    else
                    {
                        printk("Invalid version field in BCT!\n");
                        continue;
                    }
                }
                if (!pubkeyCorrect)
                {
                    memcpy(&bctEntryData[0x210], goodPubkey, sizeof(goodPubkey));
                    if (!sdmmc_storage_write(&storage, currEntry*BCT_ENTRY_SIZE_SECTORS+1, 1, &bctEntryData[SECTOR_SIZE]))
                    {
                        printk("Error writing pubkey to BOOT0!\n");
                        continue;
                    }
                    else
                    {
                        printk("GOT UN-BRICC'D!\n");
                        continue;
                    }
                }
            }
            break;
        } 
        else if (btn == BTN_VOL_DOWN)
        {
            for (u32 currEntry=0; currEntry<NUM_BCT_ENTRIES; currEntry++)
            {
                const u64 wantedMask = (uint64_t)(1u) << currEntry;
                if ((usedBctEntries & wantedMask) == 0)
                    continue;

                printk("BCT entry %u: ", currEntry);
                if ((validBctEntries & wantedMask) == 0)
                {
                    printk("ALREADY BRICC'D!\n");
                    continue;
                }

                unsigned char* bctEntryData = &bctSectors[currEntry*BCT_ENTRY_SIZE_BYTES];
                memcpy(currentHash, correctPubkeyHash, sizeof(currentHash));
                //make SURE it doesnt match
                while (memcmp(currentHash, correctPubkeyHash, sizeof(correctPubkeyHash)) == 0) 
                {
                    bctEntryData[0x210] ^= get_tmr() & 0xFF; 
                    se_calculate_sha256(currentHash, &bctEntryData[0x210], 0x100);
                }
                
                if (!sdmmc_storage_write(&storage, currEntry*BCT_ENTRY_SIZE_SECTORS+1, 1, &bctEntryData[SECTOR_SIZE]))
                    printk("Error writing to BOOT0!\n");
                else
                    printk("GOT BRICC'D!\n");
            }

            break;
        }
    }

progend:
    if (storage.sdmmc != NULL)
        sdmmc_storage_end(&storage, 0);

    mc_disable_ahb_redirect();
    if (kblobAlloc != NULL) { free(kblobAlloc); kblobAlloc = NULL; }
    if (bctAlloc != NULL) { free(bctAlloc); bctAlloc = NULL; }

    printk("\nPress the POWER button to reboot the console back into RCM.\n");
    while (btn_read() == btn) { sleep(10000); } //wait for them to release previously held button
    while (btn_read() != BTN_POWER) { sleep(10000); }
    reboot_into_rcm();

    /* Do nothing for now */
    return 0;
}
