/** @file
  Arm Ffa library common code.

  Copyright (c) 2024, Arm Limited. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

   @par Glossary:
     - FF-A - Firmware Framework for Arm A-profile

   @par Reference(s):
     - Arm Firmware Framework for Arm A-Profile v1.3 ALP1: [https://developer.arm.com/documentation/den0077/l]

**/
#include <Uefi.h>
#include <Pi/PiMultiPhase.h>

#include <Library/ArmLib.h>
#include <Library/ArmFfaLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/MmServicesTableLib.h>

#include <IndustryStandard/ArmFfaSvc.h>
#include <Guid/ArmFfaRxTxBufferInfo.h>

#include "ArmFfaCommon.h"
#include "ArmFfaRxTxMap.h"

EFI_HANDLE                 mArmFfaRxTxBufferStmmInfoHandle = NULL;
ARM_FFA_RX_TX_BUFFER_INFO  *mArmFfaRxTxBufferStmmInfo      = NULL;

/**
  Get mapped Rx/Tx buffers.

  @param [out]   TxBuffer         Address of TxBuffer
  @param [out]   TxBufferSize     Size of TxBuffer
  @param [out]   RxBuffer         Address of RxBuffer
  @param [out]   RxBufferSize     Size of RxBuffer

  @retval EFI_SUCCESS
  @retval Others             Error.

**/
EFI_STATUS
EFIAPI
ArmFfaLibGetRxTxBuffers (
  OUT VOID    **TxBuffer OPTIONAL,
  OUT UINT64  *TxBufferSize OPTIONAL,
  OUT VOID    **RxBuffer OPTIONAL,
  OUT UINT64  *RxBufferSize OPTIONAL
  )
{
  UINTN  TxBufferAddr;
  UINTN  RxBufferAddr;

  EFI_STATUS  Status = gMmst->MmLocateProtocol (
                                &gArmFfaRxTxBufferInfoGuid,
                                NULL,
                                (VOID **)&mArmFfaRxTxBufferStmmInfo
                                );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to locate Rx/Tx buffer protocol... Status: %r\n", __func__, Status));
    return Status;
  }

  TxBufferAddr = (UINTN)mArmFfaRxTxBufferStmmInfo->TxBufferAddr;
  RxBufferAddr = (UINTN)mArmFfaRxTxBufferStmmInfo->RxBufferAddr;

  if ((TxBufferAddr == 0x00) || (RxBufferAddr == 0x00)) {
    return EFI_NOT_READY;
  }

  if (TxBuffer != NULL) {
    *TxBuffer = (VOID *)TxBufferAddr;
  }

  if (TxBufferSize != NULL) {
    *TxBufferSize = mArmFfaRxTxBufferStmmInfo->TxBufferSize;
  }

  if (RxBuffer != NULL) {
    *RxBuffer = (VOID *)RxBufferAddr;
  }

  if (RxBufferSize != NULL) {
    *RxBufferSize = mArmFfaRxTxBufferStmmInfo->RxBufferSize;
  }

  return EFI_SUCCESS;
}

/**
  Mapping Rx/Tx buffers.
  This function is only called in ArmFfaLibConstructor because
  Rx/Tx buffer is registered only once per partition.

  @retval EFI_SUCCESS
  @retval EFI_ALREADY_STARTED     Rx/Tx buffer already mapped.
  @retval EFI_OUT_OF_RESOURCE     Out of memory
  @retval EFI_INVALID_PARAMETER   Invalid alignment of Rx/Tx buffer
  @retval Others                  Error

**/
EFI_STATUS
EFIAPI
ArmFfaLibRxTxMap (
  IN VOID
  )
{
  EFI_STATUS    Status;
  ARM_FFA_ARGS  FfaArgs;
  UINTN         Property1;
  UINTN         Property2;
  UINTN         MinSizeAndAlign;
  UINTN         MaxSize;
  VOID          *Buffers;
  VOID          *TxBuffer;
  VOID          *RxBuffer;
  UINT64        BufferSize;

  Status = gMmst->MmLocateProtocol (
                    &gArmFfaRxTxBufferInfoGuid,
                    NULL,
                    (VOID **)&mArmFfaRxTxBufferStmmInfo
                    );
  if (!EFI_ERROR (Status)) {
    // Great, we got what we need.
    return EFI_ALREADY_STARTED;
  }

  Status = ArmFfaLibGetFeatures (
             ARM_FID_FFA_RXTX_MAP,
             FFA_RXTX_MAP_INPUT_PROPERTY_DEFAULT,
             &Property1,
             &Property2
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: Failed to get RX/TX buffer property... Status: %r\n",
      __func__,
      Status
      ));
    return Status;
  }

  ZeroMem (&FfaArgs, sizeof (ARM_FFA_ARGS));

  MinSizeAndAlign =
    ((Property1 >>
      ARM_FFA_BUFFER_MINSIZE_AND_ALIGN_SHIFT) &
     ARM_FFA_BUFFER_MINSIZE_AND_ALIGN_MASK);

  switch (MinSizeAndAlign) {
    case ARM_FFA_BUFFER_MINSIZE_AND_ALIGN_4K:
      MinSizeAndAlign = SIZE_4KB;
      break;
    case ARM_FFA_BUFFER_MINSIZE_AND_ALIGN_16K:
      MinSizeAndAlign = SIZE_16KB;
      break;
    case ARM_FFA_BUFFER_MINSIZE_AND_ALIGN_64K:
      MinSizeAndAlign = SIZE_64KB;
      break;
    default:
      DEBUG ((DEBUG_ERROR, "%a: Invalid MinSizeAndAlign: 0x%x\n", __func__, MinSizeAndAlign));
      return EFI_UNSUPPORTED;
  }

  MaxSize = (Property1 >> ARM_FFA_BUFFER_MAXSIZE_PAGE_COUNT_SHIFT) &
            ARM_FFA_BUFFER_MAXSIZE_PAGE_COUNT_MASK;

  MaxSize = ((MaxSize == 0) ? MAX_UINTN : (MaxSize * MinSizeAndAlign));

  if ((MinSizeAndAlign > (PcdGet64 (PcdFfaTxRxPageCount) * EFI_PAGE_SIZE)) ||
      (MaxSize < (PcdGet64 (PcdFfaTxRxPageCount) * EFI_PAGE_SIZE)))
  {
    DEBUG ((
      DEBUG_ERROR,
      "%a: Buffer is too small! MinSize: 0x%x, MaxSize: 0x%x, PageCount: %d\n",
      __func__,
      MinSizeAndAlign,
      MaxSize,
      PcdGet64 (PcdFfaTxRxPageCount)
      ));
    return EFI_INVALID_PARAMETER;
  }

  Buffers = AllocateAlignedPages ((PcdGet64 (PcdFfaTxRxPageCount) * 2), MinSizeAndAlign);
  if (Buffers == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  BufferSize = PcdGet64 (PcdFfaTxRxPageCount) * EFI_PAGE_SIZE;
  TxBuffer   = Buffers;
  RxBuffer   = Buffers + BufferSize;

  FfaArgs.Arg0 = ARM_FID_FFA_RXTX_MAP;
  FfaArgs.Arg1 = (UINTN)TxBuffer;
  FfaArgs.Arg2 = (UINTN)RxBuffer;

  /*
   * PcdFfaTxRxPageCount sets with count of EFI_PAGE_SIZE granularity
   * But, PageCounts for Tx/Rx buffer should set with
   * count of Tx/Rx Buffer's MinSizeAndAlign. granularity.
   */
  FfaArgs.Arg3 = PcdGet64 (PcdFfaTxRxPageCount) / EFI_SIZE_TO_PAGES (MinSizeAndAlign);

  ArmCallFfa (&FfaArgs);

  Status = FfaArgsToEfiStatus (&FfaArgs);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: Failed to map Rx/Tx buffer. Status: %r\n",
      __func__,
      Status
      ));
    goto ErrorHandler;
  }

  mArmFfaRxTxBufferStmmInfo = AllocateZeroPool (sizeof (ARM_FFA_RX_TX_BUFFER_INFO));
  if (mArmFfaRxTxBufferStmmInfo == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto ErrorHandler;
  }

  mArmFfaRxTxBufferStmmInfo->TxBufferAddr = TxBuffer;
  mArmFfaRxTxBufferStmmInfo->RxBufferAddr = RxBuffer;
  mArmFfaRxTxBufferStmmInfo->TxBufferSize = BufferSize;
  mArmFfaRxTxBufferStmmInfo->RxBufferSize = BufferSize;

  Status = gMmst->MmInstallProtocolInterface (
                    &mArmFfaRxTxBufferStmmInfoHandle,
                    &gArmFfaRxTxBufferInfoGuid,
                    EFI_NATIVE_INTERFACE,
                    mArmFfaRxTxBufferStmmInfo
                    );

  return Status;

ErrorHandler:
  FreeAlignedPages (Buffers, (PcdGet64 (PcdFfaTxRxPageCount) * 2));
  TxBuffer = NULL;
  RxBuffer = NULL;

  return Status;
}

/**
  Unmap Rx/Tx buffer.
  This function is only called in Exit boot service because
  Rx/Tx buffer is registered only once per partition.

  @retval EFI_SUCCESS
  @retval EFI_INVALID_PARAMETERS               Already unregistered
  @retval EFI_UNSUPPORTED                      Not supported

**/
EFI_STATUS
EFIAPI
ArmFfaLibRxTxUnmap (
  IN VOID
  )
{
  EFI_STATUS    Status;
  ARM_FFA_ARGS  FfaArgs;
  VOID          *Buffers;

  if (mArmFfaRxTxBufferStmmInfoHandle == NULL) {
    // This means that the agent tried to unmap the buffers before even know them.
    // Let's be a nice player...
    return EFI_UNSUPPORTED;
  }

  ZeroMem (&FfaArgs, sizeof (ARM_FFA_ARGS));

  FfaArgs.Arg0 = ARM_FID_FFA_RXTX_UNMAP;
  FfaArgs.Arg1 = (gPartId << ARM_FFA_SOURCE_EP_SHIFT);

  ArmCallFfa (&FfaArgs);

  Status = FfaArgsToEfiStatus (&FfaArgs);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /*
   * Rx/Tx Buffer are allocated with continuous pages.
   * and start address of these pages is set on PcdFfaTxBuffer.
   * See ArmFfaLibRxTxMap().
   */
  Buffers = (VOID *)(UINTN)mArmFfaRxTxBufferStmmInfo->TxBufferAddr;
  if (Buffers != NULL) {
    FreeAlignedPages (Buffers, (EFI_SIZE_TO_PAGES (mArmFfaRxTxBufferStmmInfo->TxBufferSize) * 2));
  }

  Status = gMmst->MmUninstallProtocolInterface (
                    mArmFfaRxTxBufferStmmInfoHandle,
                    &gArmFfaRxTxBufferInfoGuid,
                    mArmFfaRxTxBufferStmmInfo
                    );

  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: Failed to uninstall Rx/Tx buffer protocol... Status: %r\n",
      __func__,
      Status
      ));
    return Status;
  }

  FreePool (mArmFfaRxTxBufferStmmInfo);
  mArmFfaRxTxBufferStmmInfo = NULL;

  return EFI_SUCCESS;
}
