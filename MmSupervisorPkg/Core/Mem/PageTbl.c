/** @file
Page Fault (#PF) handler for X64 processors

Copyright (c) 2009 - 2019, Intel Corporation. All rights reserved.<BR>
Copyright (c) 2017, AMD Incorporated. All rights reserved.<BR>

SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiMm.h>
#include <Register/Cpuid.h>
#include <Protocol/MpService.h>
#include <Protocol/SmmConfiguration.h>

#include <Library/BaseLib.h>
#include <Library/SmmCpuPlatformHookLib.h>
#include <Library/ResetSystemLib.h> // MSCHANGE - Allow system to reset instead of halt in test mode.

#include "MmSupervisorCore.h"
#include "Mem.h"
#include "SmmProfile.h"
#include "SmmProfileInternal.h"
#include "Relocate/Relocate.h"
#include "Services/CpuService/CpuService.h"
#include "Services/MpService/MpService.h"
#include "Telemetry/Telemetry.h"

#include <Library/MmMemoryProtectionHobLib.h> // MU_CHANGE

#define PAGE_TABLE_PAGES  8
#define ACC_MAX_BIT       BIT3

LIST_ENTRY                mPagePool           = INITIALIZE_LIST_HEAD_VARIABLE (mPagePool);
BOOLEAN                   m1GPageTableSupport = FALSE;
BOOLEAN                   mCpuSmmRestrictedMemoryAccess;
X86_ASSEMBLY_PATCH_LABEL  gPatch5LevelPagingNeeded;
UINT8                     mPhysicalAddressBits;

/**
  Check if 1-GByte pages is supported by processor or not.

  @retval TRUE   1-GByte pages is supported.
  @retval FALSE  1-GByte pages is not supported.

**/
BOOLEAN
Is1GPageSupport (
  VOID
  )
{
  UINT32  RegEax;
  UINT32  RegEdx;

  AsmCpuid (0x80000000, &RegEax, NULL, NULL, NULL);
  if (RegEax >= 0x80000001) {
    AsmCpuid (0x80000001, NULL, NULL, NULL, &RegEdx);
    if ((RegEdx & BIT26) != 0) {
      return TRUE;
    }
  }

  return FALSE;
}

/**
  The routine returns TRUE when CPU supports it (CPUID[7,0].ECX.BIT[16] is set) and
  the max physical address bits is bigger than 48. Because 4-level paging can support
  to address physical address up to 2^48 - 1, there is no need to enable 5-level paging
  with max physical address bits <= 48.

  @retval TRUE  5-level paging enabling is needed.
  @retval FALSE 5-level paging enabling is not needed.
**/
BOOLEAN
Is5LevelPagingNeeded (
  VOID
  )
{
  CPUID_VIR_PHY_ADDRESS_SIZE_EAX               VirPhyAddressSize;
  CPUID_STRUCTURED_EXTENDED_FEATURE_FLAGS_ECX  ExtFeatureEcx;
  UINT32                                       MaxExtendedFunctionId;

  AsmCpuid (CPUID_EXTENDED_FUNCTION, &MaxExtendedFunctionId, NULL, NULL, NULL);
  if (MaxExtendedFunctionId >= CPUID_VIR_PHY_ADDRESS_SIZE) {
    AsmCpuid (CPUID_VIR_PHY_ADDRESS_SIZE, &VirPhyAddressSize.Uint32, NULL, NULL, NULL);
  } else {
    VirPhyAddressSize.Bits.PhysicalAddressBits = 36;
  }

  AsmCpuidEx (
    CPUID_STRUCTURED_EXTENDED_FEATURE_FLAGS,
    CPUID_STRUCTURED_EXTENDED_FEATURE_FLAGS_SUB_LEAF_INFO,
    NULL,
    NULL,
    &ExtFeatureEcx.Uint32,
    NULL
    );
  DEBUG ((
    DEBUG_INFO,
    "PhysicalAddressBits = %d, 5LPageTable = %d.\n",
    VirPhyAddressSize.Bits.PhysicalAddressBits,
    ExtFeatureEcx.Bits.FiveLevelPage
    ));

  if ((VirPhyAddressSize.Bits.PhysicalAddressBits > 4 * 9 + 12) &&
      (ExtFeatureEcx.Bits.FiveLevelPage == 1))
  {
    return TRUE;
  } else {
    return FALSE;
  }
}

/**
  Get page table base address and the depth of the page table.

  @param[out] Base        Page table base address.
  @param[out] FiveLevels  TRUE means 5 level paging. FALSE means 4 level paging.
**/
VOID
GetPageTable (
  OUT UINTN    *Base,
  OUT BOOLEAN  *FiveLevels OPTIONAL
  )
{
  IA32_CR4  Cr4;

  if (mSmmCr3 == 0) {
    *Base = AsmReadCr3 () & PAGING_4K_ADDRESS_MASK_64;
    if (FiveLevels != NULL) {
      Cr4.UintN   = AsmReadCr4 ();
      *FiveLevels = (BOOLEAN)(Cr4.Bits.LA57 == 1);
    }

    return;
  }

  *Base = mSmmCr3;
  if (FiveLevels != NULL) {
    *FiveLevels = m5LevelPagingNeeded;
  }
}

/**
  Set sub-entries number in entry.

  @param[in, out] Entry        Pointer to entry
  @param[in]      SubEntryNum  Sub-entries number based on 0:
                               0 means there is 1 sub-entry under this entry
                               0x1ff means there is 512 sub-entries under this entry

**/
VOID
SetSubEntriesNum (
  IN OUT UINT64  *Entry,
  IN     UINT64  SubEntryNum
  )
{
  //
  // Sub-entries number is saved in BIT52 to BIT60 (reserved field) in Entry
  //
  *Entry = BitFieldWrite64 (*Entry, 52, 60, SubEntryNum);
}

/**
  Return sub-entries number in entry.

  @param[in] Entry        Pointer to entry

  @return Sub-entries number based on 0:
          0 means there is 1 sub-entry under this entry
          0x1ff means there is 512 sub-entries under this entry
**/
UINT64
GetSubEntriesNum (
  IN UINT64  *Entry
  )
{
  //
  // Sub-entries number is saved in BIT52 to BIT60 (reserved field) in Entry
  //
  return BitFieldRead64 (*Entry, 52, 60);
}

/**
  Calculate the maximum support address.

  @param[in] Is5LevelPagingNeeded    If 5-level paging enabling is needed.

  @return the maximum support address.
**/
UINT8
CalculateMaximumSupportAddress (
  BOOLEAN  Is5LevelPagingNeeded
  )
{
  UINT32  RegEax;
  UINT8   PhysicalAddressBits;
  VOID    *Hob;

  //
  // Get physical address bits supported.
  //
  Hob = GetFirstHob (EFI_HOB_TYPE_CPU);
  if (Hob != NULL) {
    PhysicalAddressBits = ((EFI_HOB_CPU *)Hob)->SizeOfMemorySpace;
  } else {
    AsmCpuid (0x80000000, &RegEax, NULL, NULL, NULL);
    if (RegEax >= 0x80000008) {
      AsmCpuid (0x80000008, &RegEax, NULL, NULL, NULL);
      PhysicalAddressBits = (UINT8)RegEax;
    } else {
      PhysicalAddressBits = 36;
    }
  }

  //
  // 4-level paging supports translating 48-bit linear addresses to 52-bit physical addresses.
  // Since linear addresses are sign-extended, the linear-address space of 4-level paging is:
  // [0, 2^47-1] and [0xffff8000_00000000, 0xffffffff_ffffffff].
  // So only [0, 2^47-1] linear-address range maps to the identical physical-address range when
  // 5-Level paging is disabled.
  //
  ASSERT (PhysicalAddressBits <= 52);
  if (!Is5LevelPagingNeeded && (PhysicalAddressBits > 47)) {
    PhysicalAddressBits = 47;
  }

  return PhysicalAddressBits;
}

/**
  Create PageTable for SMM use.

  @return The address of PML4 (to set CR3).

**/
UINT32
SmmInitPageTable (
  VOID
  )
{
  UINTN       PageTable;
  LIST_ENTRY  *FreePage;
  UINTN       Index;
  EFI_STATUS  Status;
  UINT64      *PdptEntry;
  UINT64      *Pml4Entry;
  UINT64      *Pml5Entry;
  UINT8       PhysicalAddressBits;

  //
  // Initialize spin lock
  //
  InitializeSpinLock (mPFLock);

  mCpuSmmRestrictedMemoryAccess = PcdGetBool (PcdCpuSmmRestrictedMemoryAccess);
  m1GPageTableSupport           = Is1GPageSupport ();
  m5LevelPagingNeeded           = Is5LevelPagingNeeded ();
  mPhysicalAddressBits          = CalculateMaximumSupportAddress (m5LevelPagingNeeded);
  if (SmmCpuFeaturesGetSmiHandlerSize () == 0) {
    PatchInstructionX86 (gPatch5LevelPagingNeeded, m5LevelPagingNeeded, 1);
  }

  if (m5LevelPagingNeeded) {
    mPagingMode = m1GPageTableSupport ? Paging5Level1GB : Paging5Level;
  } else {
    mPagingMode = m1GPageTableSupport ? Paging4Level1GB : Paging4Level;
  }

  DEBUG ((DEBUG_INFO, "5LevelPaging Needed             - %d\n", m5LevelPagingNeeded));
  DEBUG ((DEBUG_INFO, "1GPageTable Support             - %d\n", m1GPageTableSupport));
  DEBUG ((DEBUG_INFO, "PcdCpuSmmRestrictedMemoryAccess - %d\n", mCpuSmmRestrictedMemoryAccess));
  DEBUG ((DEBUG_INFO, "PhysicalAddressBits             - %d\n", mPhysicalAddressBits));
  //
  // Generate initial SMM page table.
  // Only map [0, 4G] when PcdCpuSmmRestrictedMemoryAccess is FALSE.
  //
  PhysicalAddressBits = mCpuSmmRestrictedMemoryAccess ? mPhysicalAddressBits : 32;
  PageTable           = GenSmmPageTable (mPagingMode, PhysicalAddressBits);

  if (m5LevelPagingNeeded) {
    Pml5Entry = (UINT64 *)PageTable;
    //
    // Set Pml5Entry sub-entries number for smm PF handler usage.
    //
    SetSubEntriesNum (Pml5Entry, 1);
    Pml4Entry = (UINT64 *)((*Pml5Entry) & ~mAddressEncMask & gPhyMask);
  } else {
    Pml4Entry = (UINT64 *)PageTable;
  }

  if (Pml4Entry == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Cleanup;
  }

  //
  // Set IA32_PG_PMNT bit to mask first 4 PdptEntry.
  //
  PdptEntry = (UINT64 *)((*Pml4Entry) & ~mAddressEncMask & gPhyMask);
  for (Index = 0; Index < 4; Index++) {
    PdptEntry[Index] |= IA32_PG_PMNT;
  }

  if (!mCpuSmmRestrictedMemoryAccess) {
    //
    // Set Pml4Entry sub-entries number for smm PF handler usage.
    //
    SetSubEntriesNum (Pml4Entry, 3);

    //
    // Add pages to page pool
    //
    FreePage = (LIST_ENTRY *)AllocatePageTableMemory (PAGE_TABLE_PAGES, NULL);
    if (FreePage == NULL) {
      DEBUG ((DEBUG_ERROR, "%a Failed to allocate page for FreePage!!!\n", __FUNCTION__));
      Status = EFI_OUT_OF_RESOURCES;
      goto Cleanup;
    }

    for (Index = 0; Index < PAGE_TABLE_PAGES; Index++) {
      InsertTailList (&mPagePool, FreePage);
      FreePage += EFI_PAGE_SIZE / sizeof (*FreePage);
    }
  }

  //
  // Additional SMM IDT initialization for SMM stack guard
  //
  if (FeaturePcdGet (PcdCpuSmmStackGuard)) {
    DEBUG ((DEBUG_INFO, "Initialize IDT IST field for SMM Stack Guard\n"));
    InitializeIdtIst (EXCEPT_IA32_PAGE_FAULT, 1);
  }

  //
  // Additional SMM IDT initialization for SMM CET shadow stack
  //
  if ((PcdGet32 (PcdControlFlowEnforcementPropertyMask) != 0) && mCetSupported) {
    DEBUG ((DEBUG_INFO, "Initialize IDT IST field for SMM Shadow Stack\n"));
    InitializeIdtIst (EXCEPT_IA32_PAGE_FAULT, 1);
    InitializeIdtIst (EXCEPT_IA32_MACHINE_CHECK, 1);
  }

  Status = EFI_SUCCESS;

Cleanup:
  if (EFI_ERROR (Status)) {
    ASSERT_EFI_ERROR (Status);
    if (Pml4Entry != NULL) {
      FreePages (Pml4Entry, 1);
    }

    if (Pml5Entry != NULL) {
      FreePages (Pml5Entry, 1);
    }

    if (FreePage != NULL) {
      FreePages (FreePage, PAGE_TABLE_PAGES);
    }

    return 0;
  } else {
    //
    // Return the address of PML4/PML5 (to set CR3)
    //
    return (UINT32)PageTable;
  }
}

/**
  Set access record in entry.

  @param[in, out] Entry        Pointer to entry
  @param[in]      Acc          Access record value

**/
VOID
SetAccNum (
  IN OUT UINT64  *Entry,
  IN     UINT64  Acc
  )
{
  //
  // Access record is saved in BIT9 to BIT11 (reserved field) in Entry
  //
  *Entry = BitFieldWrite64 (*Entry, 9, 11, Acc);
}

/**
  Return access record in entry.

  @param[in] Entry        Pointer to entry

  @return Access record value.

**/
UINT64
GetAccNum (
  IN UINT64  *Entry
  )
{
  //
  // Access record is saved in BIT9 to BIT11 (reserved field) in Entry
  //
  return BitFieldRead64 (*Entry, 9, 11);
}

/**
  Return and update the access record in entry.

  @param[in, out]  Entry    Pointer to entry

  @return Access record value.

**/
UINT64
GetAndUpdateAccNum (
  IN OUT UINT64  *Entry
  )
{
  UINT64  Acc;

  Acc = GetAccNum (Entry);
  if ((*Entry & IA32_PG_A) != 0) {
    //
    // If this entry has been accessed, clear access flag in Entry and update access record
    // to the initial value 7, adding ACC_MAX_BIT is to make it larger than others
    //
    *Entry &= ~(UINT64)(UINTN)IA32_PG_A;
    SetAccNum (Entry, 0x7);
    return (0x7 + ACC_MAX_BIT);
  } else {
    if (Acc != 0) {
      //
      // If the access record is not the smallest value 0, minus 1 and update the access record field
      //
      SetAccNum (Entry, Acc - 1);
    }
  }

  return Acc;
}

/**
  Reclaim free pages for PageFault handler.

  Search the whole entries tree to find the leaf entry that has the smallest
  access record value. Insert the page pointed by this leaf entry into the
  page pool. And check its upper entries if need to be inserted into the page
  pool or not.

**/
VOID
ReclaimPages (
  VOID
  )
{
  UINT64    Pml5Entry;
  UINT64    *Pml5;
  UINT64    *Pml4;
  UINT64    *Pdpt;
  UINT64    *Pdt;
  UINTN     Pml5Index;
  UINTN     Pml4Index;
  UINTN     PdptIndex;
  UINTN     PdtIndex;
  UINTN     MinPml5;
  UINTN     MinPml4;
  UINTN     MinPdpt;
  UINTN     MinPdt;
  UINT64    MinAcc;
  UINT64    Acc;
  UINT64    SubEntriesNum;
  BOOLEAN   PML4EIgnore;
  BOOLEAN   PDPTEIgnore;
  UINT64    *ReleasePageAddress;
  IA32_CR4  Cr4;
  BOOLEAN   Enable5LevelPaging;
  UINT64    PFAddress;
  UINT64    PFAddressPml5Index;
  UINT64    PFAddressPml4Index;
  UINT64    PFAddressPdptIndex;
  UINT64    PFAddressPdtIndex;

  Pml4               = NULL;
  Pdpt               = NULL;
  Pdt                = NULL;
  MinAcc             = (UINT64)-1;
  MinPml4            = (UINTN)-1;
  MinPml5            = (UINTN)-1;
  MinPdpt            = (UINTN)-1;
  MinPdt             = (UINTN)-1;
  Acc                = 0;
  ReleasePageAddress = 0;
  PFAddress          = AsmReadCr2 ();
  PFAddressPml5Index = BitFieldRead64 (PFAddress, 48, 48 + 8);
  PFAddressPml4Index = BitFieldRead64 (PFAddress, 39, 39 + 8);
  PFAddressPdptIndex = BitFieldRead64 (PFAddress, 30, 30 + 8);
  PFAddressPdtIndex  = BitFieldRead64 (PFAddress, 21, 21 + 8);

  Cr4.UintN          = AsmReadCr4 ();
  Enable5LevelPaging = (BOOLEAN)(Cr4.Bits.LA57 == 1);
  Pml5               = (UINT64 *)(UINTN)(AsmReadCr3 () & gPhyMask);

  if (!Enable5LevelPaging) {
    //
    // Create one fake PML5 entry for 4-Level Paging
    // so that the page table parsing logic only handles 5-Level page structure.
    //
    Pml5Entry = (UINTN)Pml5 | IA32_PG_P;
    Pml5      = &Pml5Entry;
  }

  //
  // First, find the leaf entry has the smallest access record value
  //
  for (Pml5Index = 0; Pml5Index < (Enable5LevelPaging ? (EFI_PAGE_SIZE / sizeof (*Pml4)) : 1); Pml5Index++) {
    if (((Pml5[Pml5Index] & IA32_PG_P) == 0) || ((Pml5[Pml5Index] & IA32_PG_PMNT) != 0)) {
      //
      // If the PML5 entry is not present or is masked, skip it
      //
      continue;
    }

    Pml4 = (UINT64 *)(UINTN)(Pml5[Pml5Index] & gPhyMask);
    for (Pml4Index = 0; Pml4Index < EFI_PAGE_SIZE / sizeof (*Pml4); Pml4Index++) {
      if (((Pml4[Pml4Index] & IA32_PG_P) == 0) || ((Pml4[Pml4Index] & IA32_PG_PMNT) != 0)) {
        //
        // If the PML4 entry is not present or is masked, skip it
        //
        continue;
      }

      Pdpt        = (UINT64 *)(UINTN)(Pml4[Pml4Index] & ~mAddressEncMask & gPhyMask);
      PML4EIgnore = FALSE;
      for (PdptIndex = 0; PdptIndex < EFI_PAGE_SIZE / sizeof (*Pdpt); PdptIndex++) {
        if (((Pdpt[PdptIndex] & IA32_PG_P) == 0) || ((Pdpt[PdptIndex] & IA32_PG_PMNT) != 0)) {
          //
          // If the PDPT entry is not present or is masked, skip it
          //
          if ((Pdpt[PdptIndex] & IA32_PG_PMNT) != 0) {
            //
            // If the PDPT entry is masked, we will ignore checking the PML4 entry
            //
            PML4EIgnore = TRUE;
          }

          continue;
        }

        if ((Pdpt[PdptIndex] & IA32_PG_PS) == 0) {
          //
          // It's not 1-GByte pages entry, it should be a PDPT entry,
          // we will not check PML4 entry more
          //
          PML4EIgnore = TRUE;
          Pdt         = (UINT64 *)(UINTN)(Pdpt[PdptIndex] & ~mAddressEncMask & gPhyMask);
          PDPTEIgnore = FALSE;
          for (PdtIndex = 0; PdtIndex < EFI_PAGE_SIZE / sizeof (*Pdt); PdtIndex++) {
            if (((Pdt[PdtIndex] & IA32_PG_P) == 0) || ((Pdt[PdtIndex] & IA32_PG_PMNT) != 0)) {
              //
              // If the PD entry is not present or is masked, skip it
              //
              if ((Pdt[PdtIndex] & IA32_PG_PMNT) != 0) {
                //
                // If the PD entry is masked, we will not PDPT entry more
                //
                PDPTEIgnore = TRUE;
              }

              continue;
            }

            if ((Pdt[PdtIndex] & IA32_PG_PS) == 0) {
              //
              // It's not 2 MByte page table entry, it should be PD entry
              // we will find the entry has the smallest access record value
              //
              PDPTEIgnore = TRUE;
              if ((PdtIndex != PFAddressPdtIndex) || (PdptIndex != PFAddressPdptIndex) ||
                  (Pml4Index != PFAddressPml4Index) || (Pml5Index != PFAddressPml5Index))
              {
                Acc = GetAndUpdateAccNum (Pdt + PdtIndex);
                if (Acc < MinAcc) {
                  //
                  // If the PD entry has the smallest access record value,
                  // save the Page address to be released
                  //
                  MinAcc             = Acc;
                  MinPml5            = Pml5Index;
                  MinPml4            = Pml4Index;
                  MinPdpt            = PdptIndex;
                  MinPdt             = PdtIndex;
                  ReleasePageAddress = Pdt + PdtIndex;
                }
              }
            }
          }

          if (!PDPTEIgnore) {
            //
            // If this PDPT entry has no PDT entries pointer to 4K Byte pages,
            // it should only has the entries point to 2 MByte Pages
            //
            if ((PdptIndex != PFAddressPdptIndex) || (Pml4Index != PFAddressPml4Index) ||
                (Pml5Index != PFAddressPml5Index))
            {
              Acc = GetAndUpdateAccNum (Pdpt + PdptIndex);
              if (Acc < MinAcc) {
                //
                // If the PDPT entry has the smallest access record value,
                // save the Page address to be released
                //
                MinAcc             = Acc;
                MinPml5            = Pml5Index;
                MinPml4            = Pml4Index;
                MinPdpt            = PdptIndex;
                MinPdt             = (UINTN)-1;
                ReleasePageAddress = Pdpt + PdptIndex;
              }
            }
          }
        }
      }

      if (!PML4EIgnore) {
        //
        // If PML4 entry has no the PDPT entry pointer to 2 MByte pages,
        // it should only has the entries point to 1 GByte Pages
        //
        if ((Pml4Index != PFAddressPml4Index) || (Pml5Index != PFAddressPml5Index)) {
          Acc = GetAndUpdateAccNum (Pml4 + Pml4Index);
          if (Acc < MinAcc) {
            //
            // If the PML4 entry has the smallest access record value,
            // save the Page address to be released
            //
            MinAcc             = Acc;
            MinPml5            = Pml5Index;
            MinPml4            = Pml4Index;
            MinPdpt            = (UINTN)-1;
            MinPdt             = (UINTN)-1;
            ReleasePageAddress = Pml4 + Pml4Index;
          }
        }
      }
    }
  }

  //
  // Make sure one PML4/PDPT/PD entry is selected
  //
  ASSERT (MinAcc != (UINT64)-1);

  //
  // Secondly, insert the page pointed by this entry into page pool and clear this entry
  //
  InsertTailList (&mPagePool, (LIST_ENTRY *)(UINTN)(*ReleasePageAddress & ~mAddressEncMask & gPhyMask));
  *ReleasePageAddress = 0;

  //
  // Lastly, check this entry's upper entries if need to be inserted into page pool
  // or not
  //
  while (TRUE) {
    if (MinPdt != (UINTN)-1) {
      //
      // If 4K Byte Page Table is released, check the PDPT entry
      //
      Pml4          = (UINT64 *)(UINTN)(Pml5[MinPml5] & gPhyMask);
      Pdpt          = (UINT64 *)(UINTN)(Pml4[MinPml4] & ~mAddressEncMask & gPhyMask);
      SubEntriesNum = GetSubEntriesNum (Pdpt + MinPdpt);
      if ((SubEntriesNum == 0) &&
          ((MinPdpt != PFAddressPdptIndex) || (MinPml4 != PFAddressPml4Index) || (MinPml5 != PFAddressPml5Index)))
      {
        //
        // Release the empty Page Directory table if there was no more 4K Byte Page Table entry
        // clear the Page directory entry
        //
        InsertTailList (&mPagePool, (LIST_ENTRY *)(UINTN)(Pdpt[MinPdpt] & ~mAddressEncMask & gPhyMask));
        Pdpt[MinPdpt] = 0;
        //
        // Go on checking the PML4 table
        //
        MinPdt = (UINTN)-1;
        continue;
      }

      //
      // Update the sub-entries filed in PDPT entry and exit
      //
      SetSubEntriesNum (Pdpt + MinPdpt, (SubEntriesNum - 1) & 0x1FF);
      break;
    }

    if (MinPdpt != (UINTN)-1) {
      //
      // One 2MB Page Table is released or Page Directory table is released, check the PML4 entry
      //
      SubEntriesNum = GetSubEntriesNum (Pml4 + MinPml4);
      if ((SubEntriesNum == 0) && ((MinPml4 != PFAddressPml4Index) || (MinPml5 != PFAddressPml5Index))) {
        //
        // Release the empty PML4 table if there was no more 1G Byte Page Table entry
        // clear the Page directory entry
        //
        InsertTailList (&mPagePool, (LIST_ENTRY *)(UINTN)(Pml4[MinPml4] & ~mAddressEncMask & gPhyMask));
        Pml4[MinPml4] = 0;
        MinPdpt       = (UINTN)-1;
        continue;
      }

      //
      // Update the sub-entries filed in PML4 entry and exit
      //
      SetSubEntriesNum (Pml4 + MinPml4, (SubEntriesNum - 1) & 0x1FF);
      break;
    }

    //
    // PLM4 table has been released before, exit it
    //
    break;
  }
}

/**
  Allocate free Page for PageFault handler use.

  @return Page address.

**/
UINT64
AllocPage (
  VOID
  )
{
  UINT64  RetVal;

  if (IsListEmpty (&mPagePool)) {
    //
    // If page pool is empty, reclaim the used pages and insert one into page pool
    //
    ReclaimPages ();
  }

  //
  // Get one free page and remove it from page pool
  //
  RetVal = (UINT64)(UINTN)mPagePool.ForwardLink;
  RemoveEntryList (mPagePool.ForwardLink);
  //
  // Clean this page and return
  //
  ZeroMem ((VOID *)(UINTN)RetVal, EFI_PAGE_SIZE);
  return RetVal;
}

/**
  Page Fault handler for SMM use.

**/
VOID
SmiDefaultPFHandler (
  VOID
  )
{
  UINT64              *PageTable;
  UINT64              *PageTableTop;
  UINT64              PFAddress;
  UINTN               StartBit;
  UINTN               EndBit;
  UINT64              PTIndex;
  UINTN               Index;
  SMM_PAGE_SIZE_TYPE  PageSize;
  UINTN               NumOfPages;
  UINTN               PageAttribute;
  EFI_STATUS          Status;
  UINT64              *UpperEntry;
  BOOLEAN             Enable5LevelPaging;
  IA32_CR4            Cr4;

  //
  // Set default SMM page attribute
  //
  PageSize      = SmmPageSize2M;
  NumOfPages    = 1;
  PageAttribute = 0;

  EndBit       = 0;
  PageTableTop = (UINT64 *)(AsmReadCr3 () & gPhyMask);
  PFAddress    = AsmReadCr2 ();

  Cr4.UintN          = AsmReadCr4 ();
  Enable5LevelPaging = (BOOLEAN)(Cr4.Bits.LA57 != 0);

  Status = GetPlatformPageTableAttribute (PFAddress, &PageSize, &NumOfPages, &PageAttribute);
  //
  // If platform not support page table attribute, set default SMM page attribute
  //
  if (Status != EFI_SUCCESS) {
    PageSize      = SmmPageSize2M;
    NumOfPages    = 1;
    PageAttribute = 0;
  }

  if (PageSize >= MaxSmmPageSizeType) {
    PageSize = SmmPageSize2M;
  }

  if (NumOfPages > 512) {
    NumOfPages = 512;
  }

  switch (PageSize) {
    case SmmPageSize4K:
      //
      // BIT12 to BIT20 is Page Table index
      //
      EndBit = 12;
      break;
    case SmmPageSize2M:
      //
      // BIT21 to BIT29 is Page Directory index
      //
      EndBit         = 21;
      PageAttribute |= (UINTN)IA32_PG_PS;
      break;
    case SmmPageSize1G:
      if (!m1GPageTableSupport) {
        DEBUG ((DEBUG_ERROR, "1-GByte pages is not supported!"));
        ASSERT (FALSE);
      }

      //
      // BIT30 to BIT38 is Page Directory Pointer Table index
      //
      EndBit         = 30;
      PageAttribute |= (UINTN)IA32_PG_PS;
      break;
    default:
      ASSERT (FALSE);
  }

  //
  // Execute-disable is enabled, set NX bit
  //
  PageAttribute |= IA32_PG_NX;

  for (Index = 0; Index < NumOfPages; Index++) {
    PageTable  = PageTableTop;
    UpperEntry = NULL;
    for (StartBit = Enable5LevelPaging ? 48 : 39; StartBit > EndBit; StartBit -= 9) {
      PTIndex = BitFieldRead64 (PFAddress, StartBit, StartBit + 8);
      if ((PageTable[PTIndex] & IA32_PG_P) == 0) {
        //
        // If the entry is not present, allocate one page from page pool for it
        //
        PageTable[PTIndex] = AllocPage () | mAddressEncMask | PAGE_ATTRIBUTE_BITS;
      } else {
        //
        // Save the upper entry address
        //
        UpperEntry = PageTable + PTIndex;
      }

      //
      // BIT9 to BIT11 of entry is used to save access record,
      // initialize value is 7
      //
      PageTable[PTIndex] |= (UINT64)IA32_PG_A;
      SetAccNum (PageTable + PTIndex, 7);
      PageTable = (UINT64 *)(UINTN)(PageTable[PTIndex] & ~mAddressEncMask & gPhyMask);
    }

    PTIndex = BitFieldRead64 (PFAddress, StartBit, StartBit + 8);
    if ((PageTable[PTIndex] & IA32_PG_P) != 0) {
      //
      // Check if the entry has already existed, this issue may occur when the different
      // size page entries created under the same entry
      //
      DEBUG ((DEBUG_ERROR, "PageTable = %lx, PTIndex = %x, PageTable[PTIndex] = %lx\n", PageTable, PTIndex, PageTable[PTIndex]));
      DEBUG ((DEBUG_ERROR, "New page table overlapped with old page table!\n"));
      ASSERT (FALSE);
    }

    //
    // Fill the new entry
    //
    PageTable[PTIndex] = ((PFAddress | mAddressEncMask) & gPhyMask & ~((1ull << EndBit) - 1)) |
                         PageAttribute | IA32_PG_A | PAGE_ATTRIBUTE_BITS;
    if (UpperEntry != NULL) {
      SetSubEntriesNum (UpperEntry, (GetSubEntriesNum (UpperEntry) + 1) & 0x1FF);
    }

    //
    // Get the next page address if we need to create more page tables
    //
    PFAddress += (1ull << EndBit);
  }
}

/**
  ThePage Fault handler wrapper for SMM use.

  @param  InterruptType    Defines the type of interrupt or exception that
                           occurred on the processor.This parameter is processor architecture specific.
  @param  SystemContext    A pointer to the processor context when
                           the interrupt occurred on the processor.
**/
VOID
EFIAPI
SmiPFHandler (
  IN EFI_EXCEPTION_TYPE  InterruptType,
  IN EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  UINTN       PFAddress;
  UINTN       GuardPageAddress;
  UINTN       ShadowStackGuardPageAddress;
  UINTN       CpuIndex;
  EFI_STATUS  Status;

  ASSERT (InterruptType == EXCEPT_IA32_PAGE_FAULT);

  AcquireSpinLock (mPFLock);

  DumpCpuContext (InterruptType, SystemContext);

  PFAddress = AsmReadCr2 ();

  if (mCpuSmmRestrictedMemoryAccess && (PFAddress >= LShiftU64 (1, (mPhysicalAddressBits - 1)))) {
    DEBUG ((DEBUG_ERROR, "Do not support address 0x%lx by processor!\n", PFAddress));
    // MSCHANGE - Allow system to reset instead of halt in test mode.
    goto HaltOrReboot;
    // CpuDeadLoop ();
    // goto Exit;
  }

  //
  // If a page fault occurs in SMRAM range, it might be in a SMM stack/shadow stack guard page,
  // or SMM page protection violation.
  //
  if ((PFAddress >= mCpuHotPlugData.SmrrBase) &&
      (PFAddress < (mCpuHotPlugData.SmrrBase + mCpuHotPlugData.SmrrSize)))
  {
    CpuIndex                    = GetCpuIndex ();
    GuardPageAddress            = (mSmmStackArrayBase + EFI_PAGE_SIZE + CpuIndex * (mSmmStackSize + mSmmShadowStackSize));
    ShadowStackGuardPageAddress = (mSmmStackArrayBase + mSmmStackSize + EFI_PAGE_SIZE + CpuIndex * (mSmmStackSize + mSmmShadowStackSize));
    if ((FeaturePcdGet (PcdCpuSmmStackGuard)) &&
        (PFAddress >= GuardPageAddress) &&
        (PFAddress < (GuardPageAddress + EFI_PAGE_SIZE)))
    {
      DEBUG ((DEBUG_ERROR, "SMM stack overflow!\n"));
    } else if ((FeaturePcdGet (PcdCpuSmmStackGuard)) &&
               (mSmmShadowStackSize > 0) &&
               (PFAddress >= ShadowStackGuardPageAddress) &&
               (PFAddress < (ShadowStackGuardPageAddress + EFI_PAGE_SIZE)))
    {
      DEBUG ((DEBUG_ERROR, "SMM shadow stack overflow!\n"));
    } else {
      if ((SystemContext.SystemContextX64->ExceptionData & IA32_PF_EC_US) != 0) {
        DEBUG ((DEBUG_ERROR, "SMM exception at supervisor (0x%lx)\n", PFAddress));
        DEBUG_CODE (
          DumpModuleInfoByIp (*(UINTN *)(UINTN)SystemContext.SystemContextX64->Rsp);
          );
      } else if ((SystemContext.SystemContextX64->ExceptionData & IA32_PF_EC_ID) != 0) {
        DEBUG ((DEBUG_ERROR, "SMM exception at execution (0x%lx)\n", PFAddress));
        DEBUG_CODE (
          DumpModuleInfoByIp (*(UINTN *)(UINTN)SystemContext.SystemContextX64->Rsp);
          );
      } else {
        DEBUG ((DEBUG_ERROR, "SMM exception at access (0x%lx)\n", PFAddress));
        DEBUG_CODE (
          DumpModuleInfoByIp ((UINTN)SystemContext.SystemContextX64->Rip);
          );
      }
    }

    // MSCHANGE - Allow system to reset instead of halt in test mode.
    goto HaltOrReboot;
    // CpuDeadLoop ();
    // goto Exit;
  }

  //
  // If a page fault occurs in non-SMRAM range.
  //
  if ((PFAddress < mCpuHotPlugData.SmrrBase) ||
      (PFAddress >= mCpuHotPlugData.SmrrBase + mCpuHotPlugData.SmrrSize))
  {
    if ((SystemContext.SystemContextX64->ExceptionData & IA32_PF_EC_ID) != 0) {
      DEBUG ((DEBUG_ERROR, "Code executed on IP(0x%lx) out of SMM range after SMM is locked!\n", PFAddress));
      DEBUG_CODE (
        DumpModuleInfoByIp (*(UINTN *)(UINTN)SystemContext.SystemContextX64->Rsp);
        );
      // MSCHANGE - Allow system to reset instead of halt in test mode.
      goto HaltOrReboot;
      // CpuDeadLoop ();
      // goto Exit;
    }

    //
    // If NULL pointer was just accessed
    //
    // MU_CHANGE START Always enforce NULL pointer check
    // if ((PcdGet8 (PcdNullPointerDetectionPropertyMask) & BIT1) != 0 &&
    //     (PFAddress < EFI_PAGE_SIZE)) {
    if (PFAddress < EFI_PAGE_SIZE) {
      // MU_CHANGE END
      DEBUG ((DEBUG_ERROR, "!!! NULL pointer access !!!\n"));
      DEBUG_CODE (
        DumpModuleInfoByIp ((UINTN)SystemContext.SystemContextX64->Rip);
        );

      // MSCHANGE - Allow system to reset instead of halt in test mode.
      goto HaltOrReboot;
      // CpuDeadLoop ();
      // goto Exit;
    }

    if (mCpuSmmRestrictedMemoryAccess && IsSmmCommBufferForbiddenAddress (PFAddress)) {
      DEBUG ((DEBUG_ERROR, "Access SMM communication forbidden address (0x%lx)!\n", PFAddress));
      DEBUG_CODE (
        DumpModuleInfoByIp ((UINTN)SystemContext.SystemContextX64->Rip);
        );
      // MU_CHANGE - Allow system to reset instead of halt in test mode.
      goto HaltOrReboot;
      // CpuDeadLoop ();
      // goto Exit;
    }

    // This is an accessible area, but check and see if the supervisor bit caused the issue
    if ((SystemContext.SystemContextX64->ExceptionData & IA32_PF_EC_US) != 0) {
      DEBUG ((DEBUG_ERROR, "Access SMM communication supervisor address (0x%lx)!\n", PFAddress));
      DEBUG_CODE (
        DumpModuleInfoByIp ((UINTN)SystemContext.SystemContextX64->Rip);
        );
      // MU_CHANGE - Allow system to reset instead of halt in test mode.
      goto HaltOrReboot;
      // CpuDeadLoop ();
      // goto Exit;
    }
  }

  {
    SmiDefaultPFHandler ();
  }

  // MSCHANGE [BEGIN] - Allow system to reset instead of halt in test mode.
  goto Exit;

HaltOrReboot:
  // Dispatch to the registered exception handlers after demotion
  Status = PrepareNReportError (InterruptType, SystemContext);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a System encountered another error during error reporting... - %r\n", __FUNCTION__, Status));
  }

  if (mSmmRebootOnException) {
    DEBUG ((DEBUG_ERROR, "%a - Reboot here in test mode.\n", __FUNCTION__));
    ResetWarm ();
    CpuDeadLoop ();
  } else {
    CpuDeadLoop ();
  }

Exit:
  ReleaseSpinLock (mPFLock);
  return;
  // MSCHANGE [END]
}

/**
  This function reads CR2 register when on-demand paging is enabled.

  @param[out]  *Cr2  Pointer to variable to hold CR2 register value.
**/
VOID
SaveCr2 (
  OUT UINTN  *Cr2
  )
{
  if (!mCpuSmmRestrictedMemoryAccess) {
    //
    // On-demand paging is enabled when access to non-SMRAM is not restricted.
    //
    *Cr2 = AsmReadCr2 ();
  }
}

/**
  This function restores CR2 register when on-demand paging is enabled.

  @param[in]  Cr2  Value to write into CR2 register.
**/
VOID
RestoreCr2 (
  IN UINTN  Cr2
  )
{
  if (!mCpuSmmRestrictedMemoryAccess) {
    //
    // On-demand paging is enabled when access to non-SMRAM is not restricted.
    //
    AsmWriteCr2 (Cr2);
  }
}

/**
  Return whether access to non-SMRAM is restricted.

  @retval TRUE  Access to non-SMRAM is restricted.
  @retval FALSE Access to non-SMRAM is not restricted.
**/
BOOLEAN
IsRestrictedMemoryAccess (
  VOID
  )
{
  return mCpuSmmRestrictedMemoryAccess;
}
