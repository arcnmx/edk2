/** @file
  Intel Processor Trace feature.

  Copyright (c) 2017, Intel Corporation. All rights reserved.<BR>
  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include "CpuCommonFeatures.h"

#define MAX_TOPA_ENTRY_COUNT 2

///
/// Processor trace buffer size selection.
///
typedef enum {
  Enum4K    = 0,
  Enum8K,
  Enum16K,
  Enum32K,
  Enum64K,
  Enum128K,
  Enum256K,
  Enum512K,
  Enum1M,
  Enum2M,
  Enum4M,
  Enum8M,
  Enum16M,
  Enum32M,
  Enum64M,
  Enum128M,
  EnumProcTraceMemDisable
} PROC_TRACE_MEM_SIZE;

///
/// Processor trace output scheme selection.
///
typedef enum {
  OutputSchemeSingleRange = 0,
  OutputSchemeToPA,
  OutputSchemeInvalid
} PROC_TRACE_OUTPUT_SCHEME;

typedef struct  {
  BOOLEAN  ProcTraceSupported;
  BOOLEAN  TopaSupported;
  BOOLEAN  SingleRangeSupported;
} PROC_TRACE_PROCESSOR_DATA;

typedef struct  {
  UINT32                      NumberOfProcessors;

  UINT8                       ProcTraceOutputScheme;  
  UINT32                      ProcTraceMemSize;

  UINTN                       *ThreadMemRegionTable;
  UINTN                       AllocatedThreads;

  UINTN                       *TopaMemArray;
  UINTN                       TopaMemArrayCount;

  PROC_TRACE_PROCESSOR_DATA   *ProcessorData;
} PROC_TRACE_DATA;

typedef struct {
  UINT64   TopaEntry[MAX_TOPA_ENTRY_COUNT];
} PROC_TRACE_TOPA_TABLE;

/**
  Prepares for the data used by CPU feature detection and initialization.

  @param[in]  NumberOfProcessors  The number of CPUs in the platform.

  @return  Pointer to a buffer of CPU related configuration data.

  @note This service could be called by BSP only.
**/
VOID *
EFIAPI
ProcTraceGetConfigData (
  IN UINTN  NumberOfProcessors
  )
{
  PROC_TRACE_DATA  *ConfigData;

  ConfigData = AllocateZeroPool (sizeof (PROC_TRACE_DATA) + sizeof (PROC_TRACE_PROCESSOR_DATA) * NumberOfProcessors);
  ASSERT (ConfigData != NULL);
  ConfigData->ProcessorData = (PROC_TRACE_PROCESSOR_DATA *) ((UINT8*) ConfigData + sizeof (PROC_TRACE_DATA));

  ConfigData->NumberOfProcessors = (UINT32) NumberOfProcessors;
  ConfigData->ProcTraceMemSize = PcdGet32 (PcdCpuProcTraceMemSize);
  ConfigData->ProcTraceOutputScheme = PcdGet8 (PcdCpuProcTraceOutputScheme);

  return ConfigData;
}

/**
  Detects if Intel Processor Trace feature supported on current 
  processor.

  @param[in]  ProcessorNumber  The index of the CPU executing this function.
  @param[in]  CpuInfo          A pointer to the REGISTER_CPU_FEATURE_INFORMATION
                               structure for the CPU executing this function.
  @param[in]  ConfigData       A pointer to the configuration buffer returned
                               by CPU_FEATURE_GET_CONFIG_DATA.  NULL if
                               CPU_FEATURE_GET_CONFIG_DATA was not provided in
                               RegisterCpuFeature().

  @retval TRUE     Processor Trace feature is supported.
  @retval FALSE    Processor Trace feature is not supported.

  @note This service could be called by BSP/APs.
**/
BOOLEAN
EFIAPI
ProcTraceSupport (
  IN UINTN                             ProcessorNumber,
  IN REGISTER_CPU_FEATURE_INFORMATION  *CpuInfo,
  IN VOID                              *ConfigData  OPTIONAL
  )
{
  PROC_TRACE_DATA                             *ProcTraceData;
  CPUID_STRUCTURED_EXTENDED_FEATURE_FLAGS_EBX Ebx;
  CPUID_INTEL_PROCESSOR_TRACE_MAIN_LEAF_ECX   Ecx;

  //
  // Check if ProcTraceMemorySize option is enabled (0xFF means disable by user)
  //
  ProcTraceData = (PROC_TRACE_DATA *) ConfigData;
  if ((ProcTraceData->ProcTraceMemSize >= EnumProcTraceMemDisable) ||
      (ProcTraceData->ProcTraceOutputScheme >= OutputSchemeInvalid)) {
    return FALSE;
  }

  //
  // Check if Processor Trace is supported
  //
  AsmCpuidEx (CPUID_STRUCTURED_EXTENDED_FEATURE_FLAGS, 0, NULL, &Ebx.Uint32, NULL, NULL);
  ProcTraceData->ProcessorData[ProcessorNumber].ProcTraceSupported = (BOOLEAN) (Ebx.Bits.IntelProcessorTrace == 1);
  if (!ProcTraceData->ProcessorData[ProcessorNumber].ProcTraceSupported) {
    return FALSE;
  }

  AsmCpuidEx (CPUID_INTEL_PROCESSOR_TRACE, CPUID_INTEL_PROCESSOR_TRACE_MAIN_LEAF, NULL, NULL, &Ecx.Uint32, NULL);
  ProcTraceData->ProcessorData[ProcessorNumber].TopaSupported = (BOOLEAN) (Ecx.Bits.RTIT == 1);
  ProcTraceData->ProcessorData[ProcessorNumber].SingleRangeSupported = (BOOLEAN) (Ecx.Bits.SingleRangeOutput == 1);
  if (ProcTraceData->ProcessorData[ProcessorNumber].TopaSupported || 
      ProcTraceData->ProcessorData[ProcessorNumber].SingleRangeSupported) {
    return TRUE;
  }

  return FALSE;
}

/**
  Initializes Intel Processor Trace feature to specific state.

  @param[in]  ProcessorNumber  The index of the CPU executing this function.
  @param[in]  CpuInfo          A pointer to the REGISTER_CPU_FEATURE_INFORMATION
                               structure for the CPU executing this function.
  @param[in]  ConfigData       A pointer to the configuration buffer returned
                               by CPU_FEATURE_GET_CONFIG_DATA.  NULL if
                               CPU_FEATURE_GET_CONFIG_DATA was not provided in
                               RegisterCpuFeature().
  @param[in]  State            If TRUE, then the Processor Trace feature must be
                               enabled.
                               If FALSE, then the Processor Trace feature must be
                               disabled.

  @retval RETURN_SUCCESS       Intel Processor Trace feature is initialized.

**/
RETURN_STATUS
EFIAPI
ProcTraceInitialize (
  IN UINTN                             ProcessorNumber,
  IN REGISTER_CPU_FEATURE_INFORMATION  *CpuInfo,
  IN VOID                              *ConfigData,  OPTIONAL
  IN BOOLEAN                           State
  )
{
  UINT64                               MsrValue;
  UINT32                               MemRegionSize;
  UINTN                                Pages;
  UINTN                                Alignment;
  UINTN                                MemRegionBaseAddr;
  UINTN                                *ThreadMemRegionTable;
  UINTN                                Index;
  UINTN                                TopaTableBaseAddr;
  UINTN                                AlignedAddress;
  UINTN                                *TopaMemArray;
  PROC_TRACE_TOPA_TABLE                *TopaTable;
  PROC_TRACE_DATA                      *ProcTraceData;
  BOOLEAN                              FirstIn;

  ProcTraceData = (PROC_TRACE_DATA *) ConfigData;

  MemRegionBaseAddr = 0;
  FirstIn = FALSE;

  if (ProcTraceData->ThreadMemRegionTable == NULL) {
    FirstIn = TRUE;
    DEBUG ((DEBUG_INFO, "Initialize Processor Trace\n"));
  }

  ///
  /// Refer to PROC_TRACE_MEM_SIZE Table for Size Encoding
  ///
  MemRegionSize = (UINT32) (1 << (ProcTraceData->ProcTraceMemSize + 12));
  if (FirstIn) {
    DEBUG ((DEBUG_INFO, "ProcTrace: MemSize requested: 0x%X \n", MemRegionSize));
  }

  //
  // Clear MSR_IA32_RTIT_CTL[0] and IA32_RTIT_STS only if MSR_IA32_RTIT_CTL[0]==1b
  //
  MsrValue = AsmReadMsr64 (MSR_IA32_RTIT_CTL);
  if ((MsrValue & BIT0) != 0) {
    ///
    /// Clear bit 0 in MSR IA32_RTIT_CTL (570)
    ///
    MsrValue &= (UINT64) ~BIT0;
    CPU_REGISTER_TABLE_WRITE64 (
      ProcessorNumber,
      Msr,
      MSR_IA32_RTIT_CTL,
      MsrValue
      );

    ///
    /// Clear MSR IA32_RTIT_STS (571h) to all zeros
    ///
    MsrValue = AsmReadMsr64 (MSR_IA32_RTIT_STATUS);
    MsrValue &= 0x0;
    CPU_REGISTER_TABLE_WRITE64 (
      ProcessorNumber,
      Msr,
      MSR_IA32_RTIT_STATUS,
      MsrValue
      );
  }

  if (FirstIn) {
    //
    //   Let BSP allocate and create the necessary memory region (Aligned to the size of
    //   the memory region from setup option(ProcTraceMemSize) which is an integral multiple of 4kB)
    //   for the all the enabled threads for storing Processor Trace debug data. Then Configure the trace
    //   address base in MSR, IA32_RTIT_OUTPUT_BASE (560h) bits 47:12. Note that all regions must be
    //   aligned based on their size, not just 4K. Thus a 2M region must have bits 20:12 clear.
    //
    ThreadMemRegionTable = (UINTN *) AllocatePool (ProcTraceData->NumberOfProcessors * sizeof (UINTN *));
    if (ThreadMemRegionTable == NULL) {
      DEBUG ((DEBUG_ERROR, "Allocate ProcTrace ThreadMemRegionTable Failed\n"));
      return RETURN_OUT_OF_RESOURCES;
    }
    ProcTraceData->ThreadMemRegionTable = ThreadMemRegionTable;

    for (Index = 0; Index < ProcTraceData->NumberOfProcessors; Index++, ProcTraceData->AllocatedThreads++) {
      Pages = EFI_SIZE_TO_PAGES (MemRegionSize);
      Alignment = MemRegionSize;
      AlignedAddress = (UINTN) AllocateAlignedReservedPages (Pages, Alignment);
      if (AlignedAddress == 0) {
        DEBUG ((DEBUG_ERROR, "ProcTrace: Out of mem, allocated only for %d threads\n", ProcTraceData->AllocatedThreads));
        if (Index == 0) {
          //
          // Could not allocate for BSP even
          //
          FreePool ((VOID *) ThreadMemRegionTable);
          ThreadMemRegionTable = NULL;
          return RETURN_OUT_OF_RESOURCES;
        }
        break;
      }

      ThreadMemRegionTable[Index] = AlignedAddress;
      DEBUG ((DEBUG_INFO, "ProcTrace: PT MemRegionBaseAddr(aligned) for thread %d: 0x%llX \n", Index, (UINT64) ThreadMemRegionTable[Index]));
    }

    DEBUG ((DEBUG_INFO, "ProcTrace: Allocated PT mem for %d thread \n", ProcTraceData->AllocatedThreads));
    MemRegionBaseAddr = ThreadMemRegionTable[0];
  } else {
    if (ProcessorNumber < ProcTraceData->AllocatedThreads) {
      MemRegionBaseAddr = ProcTraceData->ThreadMemRegionTable[ProcessorNumber];
    } else {
      return RETURN_SUCCESS;
    }
  }

  ///
  /// Check Processor Trace output scheme: Single Range output or ToPA table
  ///

  //
  //  Single Range output scheme
  //
  if (ProcTraceData->ProcessorData[ProcessorNumber].SingleRangeSupported && 
      (ProcTraceData->ProcTraceOutputScheme == OutputSchemeSingleRange)) {
    if (FirstIn) {
      DEBUG ((DEBUG_INFO, "ProcTrace: Enabling Single Range Output scheme \n"));
    }

    //
    // Clear MSR IA32_RTIT_CTL (0x570) ToPA (Bit 8)
    //
    MsrValue = AsmReadMsr64 (MSR_IA32_RTIT_CTL);
    MsrValue &= (UINT64) ~BIT8;
    CPU_REGISTER_TABLE_WRITE64 (
      ProcessorNumber,
      Msr,
      MSR_IA32_RTIT_CTL,
      MsrValue
      );

    //
    // Program MSR IA32_RTIT_OUTPUT_BASE (0x560) bits[47:12] with the allocated Memory Region
    //
    MsrValue = (UINT64) MemRegionBaseAddr;
    CPU_REGISTER_TABLE_WRITE64 (
      ProcessorNumber,
      Msr,
      MSR_IA32_RTIT_OUTPUT_BASE,
      MsrValue
      );

    //
    // Program the Mask bits for the Memory Region to MSR IA32_RTIT_OUTPUT_MASK_PTRS (561h)
    //
    MsrValue = (UINT64) MemRegionSize - 1;
    CPU_REGISTER_TABLE_WRITE64 (
      ProcessorNumber,
      Msr,
      MSR_IA32_RTIT_OUTPUT_MASK_PTRS,
      MsrValue
      );

  }

  //
  //  ToPA(Table of physical address) scheme
  //
  if (ProcTraceData->ProcessorData[ProcessorNumber].TopaSupported && 
      (ProcTraceData->ProcTraceOutputScheme == OutputSchemeToPA)) {
    //
    //  Create ToPA structure aligned at 4KB for each logical thread
    //  with at least 2 entries by 8 bytes size each. The first entry
    //  should have the trace output base address in bits 47:12, 6:9
    //  for Size, bits 4,2 and 0 must be cleared. The second entry
    //  should have the base address of the table location in bits
    //  47:12, bits 4 and 2 must be cleared and bit 0 must be set.
    //
    if (FirstIn) {
      DEBUG ((DEBUG_INFO, "ProcTrace: Enabling ToPA scheme \n"));
      //
      // Let BSP allocate ToPA table mem for all threads
      //
      TopaMemArray = (UINTN *) AllocatePool (ProcTraceData->AllocatedThreads * sizeof (UINTN *));
      if (TopaMemArray == NULL) {
        DEBUG ((DEBUG_ERROR, "ProcTrace: Allocate mem for ToPA Failed\n"));
        return RETURN_OUT_OF_RESOURCES;
      }
      ProcTraceData->TopaMemArray = TopaMemArray;

      for (Index = 0; Index < ProcTraceData->AllocatedThreads; Index++) {
        Pages = EFI_SIZE_TO_PAGES (sizeof (PROC_TRACE_TOPA_TABLE));
        Alignment = 0x1000;
        AlignedAddress = (UINTN) AllocateAlignedReservedPages (Pages, Alignment);
        if (AlignedAddress == 0) {
          if (Index < ProcTraceData->AllocatedThreads) {
            ProcTraceData->AllocatedThreads = Index;
          }
          DEBUG ((DEBUG_ERROR, "ProcTrace:  Out of mem, allocating ToPA mem only for %d threads\n", ProcTraceData->AllocatedThreads));
          if (Index == 0) {
            //
            // Could not allocate for BSP
            //
            FreePool ((VOID *) TopaMemArray);
            TopaMemArray = NULL;
            return RETURN_OUT_OF_RESOURCES;
          }
          break;
        }

        TopaMemArray[Index] = AlignedAddress;
        DEBUG ((DEBUG_INFO, "ProcTrace: Topa table address(aligned) for thread %d is 0x%llX \n", Index,  (UINT64) TopaMemArray[Index]));
      }

      DEBUG ((DEBUG_INFO, "ProcTrace: Allocated ToPA mem for %d thread \n", ProcTraceData->AllocatedThreads));
      //
      // BSP gets the first block
      //
      TopaTableBaseAddr = TopaMemArray[0];
    } else {
      //
      // Count for currently executing AP.
      //
      if (ProcessorNumber < ProcTraceData->AllocatedThreads) {
        TopaTableBaseAddr = ProcTraceData->TopaMemArray[ProcessorNumber];
      } else {
        return RETURN_SUCCESS;
      }
    }

    TopaTable = (PROC_TRACE_TOPA_TABLE *) TopaTableBaseAddr;
    TopaTable->TopaEntry[0] = (UINT64) (MemRegionBaseAddr | ((ProcTraceData->ProcTraceMemSize) << 6)) & ~BIT0;
    TopaTable->TopaEntry[1] = (UINT64) TopaTableBaseAddr | BIT0;

    //
    // Program the MSR IA32_RTIT_OUTPUT_BASE (0x560) bits[47:12] with ToPA base
    //
    MsrValue = (UINT64) TopaTableBaseAddr;
    CPU_REGISTER_TABLE_WRITE64 (
      ProcessorNumber,
      Msr,
      MSR_IA32_RTIT_OUTPUT_BASE,
      MsrValue
      );

    //
    // Set the MSR IA32_RTIT_OUTPUT_MASK (0x561) bits[63:7] to 0
    //
    CPU_REGISTER_TABLE_WRITE64 (
      ProcessorNumber,
      Msr,
      MSR_IA32_RTIT_OUTPUT_MASK_PTRS,
      0x7F
      );
    //
    // Enable ToPA output scheme by enabling MSR IA32_RTIT_CTL (0x570) ToPA (Bit 8)
    //
    MsrValue = AsmReadMsr64 (MSR_IA32_RTIT_CTL);
    MsrValue |= BIT8;
    CPU_REGISTER_TABLE_WRITE64 (
      ProcessorNumber,
      Msr,
      MSR_IA32_RTIT_CTL,
      MsrValue
      );
  }

  ///
  /// Enable the Processor Trace feature from MSR IA32_RTIT_CTL (570h)
  ///
  MsrValue = AsmReadMsr64 (MSR_IA32_RTIT_CTL);
  MsrValue |= (UINT64) BIT0 + BIT2 + BIT3 + BIT13;
  if (!State) {
    MsrValue &= (UINT64) ~BIT0;
  }
  CPU_REGISTER_TABLE_WRITE64 (
    ProcessorNumber,
    Msr,
    MSR_IA32_RTIT_CTL,
    MsrValue
    );

  return RETURN_SUCCESS;
}
