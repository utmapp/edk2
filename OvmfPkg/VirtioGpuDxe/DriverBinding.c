/** @file

  Implement the Driver Binding Protocol and the Component Name 2 Protocol for
  the Virtio GPU hybrid driver.

  Copyright (C) 2016, Red Hat, Inc.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/ComponentName2.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/PciIo.h>

#include "VirtioGpu.h"

//
// The device path node that describes the Video Output Device Attributes for
// the single head (UEFI child handle) that we support.
//
// The ACPI_DISPLAY_ADR() macro corresponds to Table B-2, section "B.4.2 _DOD"
// in the ACPI 3.0b spec, or more recently, to Table B-379, section "B.3.2
// _DOD" in the ACPI 6.0 spec.
//
STATIC CONST ACPI_ADR_DEVICE_PATH mAcpiAdr = {
  {                                         // Header
    ACPI_DEVICE_PATH,                       //   Type
    ACPI_ADR_DP,                            //   SubType
    { sizeof mAcpiAdr, 0 },                 //   Length
  },
  ACPI_DISPLAY_ADR (                        // ADR
    1,                                      //   DeviceIdScheme: use the ACPI
                                            //     bit-field definitions
    0,                                      //   HeadId
    0,                                      //   NonVgaOutput
    1,                                      //   BiosCanDetect
    0,                                      //   VendorInfo
    ACPI_ADR_DISPLAY_TYPE_EXTERNAL_DIGITAL, //   Type
    0,                                      //   Port
    0                                       //   Index
    )
};

//
// Macro for casting VGPU_GOP.Gop to VGPU_GOP.
//
#define VGPU_GOP_FROM_GOP(GopPointer) \
          CR (GopPointer, VGPU_GOP, Gop, VGPU_GOP_SIG)

#define RAMFB_FORMAT  0x34325258 /* DRM_FORMAT_XRGB8888 */
#define RAMFB_BPP     4

#pragma pack (1)
typedef struct RAMFB_CONFIG {
  UINT64 Address;
  UINT32 FourCC;
  UINT32 Flags;
  UINT32 Width;
  UINT32 Height;
  UINT32 Stride;
} RAMFB_CONFIG;
#pragma pack ()

STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION mQemuRamfbModeInfo[] = {
  {
    0,    // Version
    640,  // HorizontalResolution
    480,  // VerticalResolution
  },{
    0,    // Version
    800,  // HorizontalResolution
    600,  // VerticalResolution
  },{
    0,    // Version
    1024, // HorizontalResolution
    768,  // VerticalResolution
  }
};

STATIC
EFI_STATUS
EFIAPI
QemuRamfbGraphicsOutputQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  VGPU_GOP                              *VgpuGop;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *ModeInfo;

  VgpuGop = VGPU_GOP_FROM_GOP (This);
  if (Info == NULL || SizeOfInfo == NULL ||
      ModeNumber >= VgpuGop->GopMode.MaxMode) {
    return EFI_INVALID_PARAMETER;
  }
  ModeInfo = &mQemuRamfbModeInfo[ModeNumber];

  *Info = AllocateCopyPool (sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),
            ModeInfo);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  *SizeOfInfo = sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
QemuRamfbGraphicsOutputSetMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
  IN  UINT32                       ModeNumber
  )
{
  VGPU_GOP                              *VgpuGop;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *ModeInfo;
  RAMFB_CONFIG                          Config;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL         Black;
  RETURN_STATUS                         Status;

  VgpuGop = VGPU_GOP_FROM_GOP (This);
  if (ModeNumber >= VgpuGop->GopMode.MaxMode) {
    return EFI_UNSUPPORTED;
  }
  ModeInfo = &mQemuRamfbModeInfo[ModeNumber];

  DEBUG ((DEBUG_INFO, "Ramfb: SetMode %u (%ux%u)\n", ModeNumber,
    ModeInfo->HorizontalResolution, ModeInfo->VerticalResolution));

  Config.Address = SwapBytes64 (VgpuGop->GopMode.FrameBufferBase);
  Config.FourCC  = SwapBytes32 (RAMFB_FORMAT);
  Config.Flags   = SwapBytes32 (0);
  Config.Width   = SwapBytes32 (ModeInfo->HorizontalResolution);
  Config.Height  = SwapBytes32 (ModeInfo->VerticalResolution);
  Config.Stride  = SwapBytes32 (ModeInfo->HorizontalResolution * RAMFB_BPP);

  Status = FrameBufferBltConfigure (
             (VOID*)(UINTN)VgpuGop->GopMode.FrameBufferBase,
             ModeInfo,
             VgpuGop->QemuRamfbFrameBufferBltConfigure,
             &VgpuGop->QemuRamfbFrameBufferBltConfigureSize
             );

  if (Status == RETURN_BUFFER_TOO_SMALL) {
    if (VgpuGop->QemuRamfbFrameBufferBltConfigure != NULL) {
      FreePool (VgpuGop->QemuRamfbFrameBufferBltConfigure);
    }
   VgpuGop->QemuRamfbFrameBufferBltConfigure =
      AllocatePool (VgpuGop->QemuRamfbFrameBufferBltConfigureSize);
    if (VgpuGop->QemuRamfbFrameBufferBltConfigure == NULL) {
      VgpuGop->QemuRamfbFrameBufferBltConfigureSize = 0;
      return EFI_OUT_OF_RESOURCES;
    }

    Status = FrameBufferBltConfigure (
               (VOID*)(UINTN)VgpuGop->GopMode.FrameBufferBase,
               ModeInfo,
               VgpuGop->QemuRamfbFrameBufferBltConfigure,
               &VgpuGop->QemuRamfbFrameBufferBltConfigureSize
               );
  }
  if (RETURN_ERROR (Status)) {
    ASSERT (Status == RETURN_UNSUPPORTED);
    return Status;
  }

  VgpuGop->GopMode.Mode = ModeNumber;
  VgpuGop->GopMode.Info = ModeInfo;
  VgpuGop->GopMode.SizeOfInfo = sizeof VgpuGop->GopModeInfo;
  VgpuGop->Gop.Mode = &VgpuGop->GopMode;

  QemuFwCfgSelectItem (VgpuGop->RamfbFwCfgItem);
  QemuFwCfgWriteBytes (sizeof (Config), &Config);

  //
  // clear screen
  //
  ZeroMem (&Black, sizeof (Black));
  Status = FrameBufferBlt (
             VgpuGop->QemuRamfbFrameBufferBltConfigure,
             &Black,
             EfiBltVideoFill,
             0,                               // SourceX -- ignored
             0,                               // SourceY -- ignored
             0,                               // DestinationX
             0,                               // DestinationY
             ModeInfo->HorizontalResolution,  // Width
             ModeInfo->VerticalResolution,    // Height
             0                                // Delta -- ignored
             );
  if (RETURN_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: clearing the screen failed: %r\n",
      __FUNCTION__, Status));
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
QemuRamfbGraphicsOutputBlt (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  EFI_GRAPHICS_OUTPUT_BLT_PIXEL         *BltBuffer, OPTIONAL
  IN  EFI_GRAPHICS_OUTPUT_BLT_OPERATION     BltOperation,
  IN  UINTN                                 SourceX,
  IN  UINTN                                 SourceY,
  IN  UINTN                                 DestinationX,
  IN  UINTN                                 DestinationY,
  IN  UINTN                                 Width,
  IN  UINTN                                 Height,
  IN  UINTN                                 Delta
  )
{
  VGPU_GOP *VgpuGop;

  VgpuGop = VGPU_GOP_FROM_GOP (This);
  return FrameBufferBlt (
           VgpuGop->QemuRamfbFrameBufferBltConfigure,
           BltBuffer,
           BltOperation,
           SourceX,
           SourceY,
           DestinationX,
           DestinationY,
           Width,
           Height,
           Delta
           );
}

STATIC CONST EFI_GRAPHICS_OUTPUT_PROTOCOL mQemuRamfbGraphicsOutput = {
  QemuRamfbGraphicsOutputQueryMode,
  QemuRamfbGraphicsOutputSetMode,
  QemuRamfbGraphicsOutputBlt,
  NULL,
};

//
// Component Name 2 Protocol implementation.
//
STATIC CONST EFI_UNICODE_STRING_TABLE mDriverNameTable[] = {
  { "en", L"Virtio GPU Driver" },
  { NULL, NULL                 }
};

STATIC
EFI_STATUS
EFIAPI
VirtioGpuGetDriverName (
  IN  EFI_COMPONENT_NAME2_PROTOCOL *This,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **DriverName
  )
{
  return LookupUnicodeString2 (Language, This->SupportedLanguages,
           mDriverNameTable, DriverName, FALSE /* Iso639Language */);
}

STATIC
EFI_STATUS
EFIAPI
VirtioGpuGetControllerName (
  IN  EFI_COMPONENT_NAME2_PROTOCOL *This,
  IN  EFI_HANDLE                   ControllerHandle,
  IN  EFI_HANDLE                   ChildHandle       OPTIONAL,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **ControllerName
  )
{
  EFI_STATUS Status;
  VGPU_DEV   *VgpuDev;

  //
  // Look up the VGPU_DEV "protocol interface" on ControllerHandle.
  //
  Status = gBS->OpenProtocol (ControllerHandle, &gEfiCallerIdGuid,
                  (VOID **)&VgpuDev, gImageHandle, ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  //
  // Sanity check: if we found gEfiCallerIdGuid on ControllerHandle, then we
  // keep its Virtio Device Protocol interface open BY_DRIVER.
  //
  ASSERT_EFI_ERROR (EfiTestManagedDevice (ControllerHandle, gImageHandle,
                      &gVirtioDeviceProtocolGuid));

  if (ChildHandle == NULL) {
    //
    // The caller is querying the name of the VGPU_DEV controller.
    //
    return LookupUnicodeString2 (Language, This->SupportedLanguages,
             VgpuDev->BusName, ControllerName, FALSE /* Iso639Language */);
  }

  //
  // Otherwise, the caller is looking for the name of the GOP child controller.
  // Check if it is asking about the GOP child controller that we manage. (The
  // condition below covers the case when we haven't produced the GOP child
  // controller yet, or we've destroyed it since.)
  //
  if (VgpuDev->Child == NULL || ChildHandle != VgpuDev->Child->GopHandle) {
    return EFI_UNSUPPORTED;
  }
  //
  // Sanity check: our GOP child controller keeps the VGPU_DEV controller's
  // Virtio Device Protocol interface open BY_CHILD_CONTROLLER.
  //
  ASSERT_EFI_ERROR (EfiTestChildHandle (ControllerHandle, ChildHandle,
                      &gVirtioDeviceProtocolGuid));

  return LookupUnicodeString2 (Language, This->SupportedLanguages,
           VgpuDev->Child->GopName, ControllerName,
           FALSE /* Iso639Language */);
}

STATIC CONST EFI_COMPONENT_NAME2_PROTOCOL mComponentName2 = {
  VirtioGpuGetDriverName,
  VirtioGpuGetControllerName,
  "en"                       // SupportedLanguages (RFC 4646)
};

//
// Helper functions for the Driver Binding Protocol Implementation.
//
/**
  Format the VGPU_DEV controller name, to be looked up and returned by
  VirtioGpuGetControllerName().

  @param[in] ControllerHandle  The handle that identifies the VGPU_DEV
                               controller.

  @param[in] AgentHandle       The handle of the agent that will attempt to
                               temporarily open the PciIo protocol. This is the
                               DriverBindingHandle member of the
                               EFI_DRIVER_BINDING_PROTOCOL whose Start()
                               function is calling this function.

  @param[in] DevicePath        The device path that is installed on
                               ControllerHandle.

  @param[out] ControllerName   A dynamically allocated unicode string that
                               unconditionally says "Virtio GPU Device", with a
                               PCI Segment:Bus:Device.Function location
                               optionally appended. The latter part is only
                               produced if DevicePath contains at least one
                               PciIo node; in that case, the most specific such
                               node is used for retrieving the location info.
                               The caller is responsible for freeing
                               ControllerName after use.

  @retval EFI_SUCCESS           ControllerName has been formatted.

  @retval EFI_OUT_OF_RESOURCES  Failed to allocate memory for ControllerName.
**/
STATIC
EFI_STATUS
FormatVgpuDevName (
  IN  EFI_HANDLE               ControllerHandle,
  IN  EFI_HANDLE               AgentHandle,
  IN  EFI_DEVICE_PATH_PROTOCOL *DevicePath,
  OUT CHAR16                   **ControllerName
  )
{
  EFI_HANDLE          PciIoHandle;
  EFI_PCI_IO_PROTOCOL *PciIo;
  UINTN               Segment, Bus, Device, Function;
  STATIC CONST CHAR16 ControllerNameStem[] = L"Virtio GPU Device";
  UINTN               ControllerNameSize;

  if (EFI_ERROR (gBS->LocateDevicePath (&gEfiPciIoProtocolGuid, &DevicePath,
                        &PciIoHandle)) ||
      EFI_ERROR (gBS->OpenProtocol (PciIoHandle, &gEfiPciIoProtocolGuid,
                        (VOID **)&PciIo, AgentHandle, ControllerHandle,
                        EFI_OPEN_PROTOCOL_GET_PROTOCOL)) ||
      EFI_ERROR (PciIo->GetLocation (PciIo, &Segment, &Bus, &Device,
                          &Function))) {
    //
    // Failed to retrieve location info, return verbatim copy of static string.
    //
    *ControllerName = AllocateCopyPool (sizeof ControllerNameStem,
                        ControllerNameStem);
    return (*ControllerName == NULL) ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
  }
  //
  // Location info available, format ControllerName dynamically.
  //
  ControllerNameSize = sizeof ControllerNameStem + // includes L'\0'
                       sizeof (CHAR16) * (1 + 4 +  // Segment
                                          1 + 2 +  // Bus
                                          1 + 2 +  // Device
                                          1 + 1    // Function
                                          );
  *ControllerName = AllocatePool (ControllerNameSize);
  if (*ControllerName == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  UnicodeSPrintAsciiFormat (*ControllerName, ControllerNameSize,
    "%s %04x:%02x:%02x.%x", ControllerNameStem, (UINT32)Segment, (UINT32)Bus,
    (UINT32)Device, (UINT32)Function);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
InitQemuRamfbGop (
  IN OUT VGPU_DEV                 *ParentBus,
  IN     EFI_DEVICE_PATH_PROTOCOL *ParentDevicePath,
  IN     EFI_HANDLE               ParentBusController,
  IN     EFI_HANDLE               DriverBindingHandle
  )
{
  VGPU_GOP                  *VgpuGop;
  EFI_STATUS                Status;
  CHAR16                    *ParentBusName;
  STATIC CONST CHAR16       NameSuffix[] = L" RAMFB Head #0";
  UINTN                     NameSize;
  CHAR16                    *Name;
  VOID                      *ParentVirtIo;
  FIRMWARE_CONFIG_ITEM      RamfbFwCfgItem;
  EFI_PHYSICAL_ADDRESS      FbBase;
  UINTN                     FbSize, MaxFbSize, Pages;
  UINTN                     FwCfgSize;
  UINTN                     Index;

  if (!QemuFwCfgIsAvailable ()) {
    DEBUG ((DEBUG_INFO, "Ramfb: no FwCfg\n"));
    return EFI_NOT_FOUND;
  }

  Status = QemuFwCfgFindFile ("etc/ramfb", &RamfbFwCfgItem, &FwCfgSize);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }
  if (FwCfgSize != sizeof (RAMFB_CONFIG)) {
    DEBUG ((DEBUG_ERROR, "Ramfb: FwCfg size mismatch (expected %lu, got %lu)\n",
      (UINT64)sizeof (RAMFB_CONFIG), (UINT64)FwCfgSize));
    return EFI_PROTOCOL_ERROR;
  }

  MaxFbSize = 0;
  for (Index = 0; Index < ARRAY_SIZE (mQemuRamfbModeInfo); Index++) {
    mQemuRamfbModeInfo[Index].PixelsPerScanLine =
      mQemuRamfbModeInfo[Index].HorizontalResolution;
    mQemuRamfbModeInfo[Index].PixelFormat =
      PixelBlueGreenRedReserved8BitPerColor;
    FbSize = RAMFB_BPP *
      mQemuRamfbModeInfo[Index].HorizontalResolution *
      mQemuRamfbModeInfo[Index].VerticalResolution;
    if (MaxFbSize < FbSize) {
      MaxFbSize = FbSize;
    }
    DEBUG ((DEBUG_INFO, "Ramfb: Mode %lu: %ux%u, %lu kB\n", (UINT64)Index,
      mQemuRamfbModeInfo[Index].HorizontalResolution,
      mQemuRamfbModeInfo[Index].VerticalResolution,
      (UINT64)(FbSize / 1024)));
  }

  VgpuGop = AllocateZeroPool (sizeof *VgpuGop);
  if (VgpuGop == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  VgpuGop->Signature = VGPU_GOP_SIG;
  VgpuGop->ParentBus = ParentBus;
  VgpuGop->UseRamfb = TRUE;
  VgpuGop->RamfbFwCfgItem = RamfbFwCfgItem;

  Pages = EFI_SIZE_TO_PAGES (MaxFbSize);
  MaxFbSize = EFI_PAGES_TO_SIZE (Pages);
  FbBase = (EFI_PHYSICAL_ADDRESS)(UINTN)AllocateReservedPages (Pages);
  if (FbBase == 0) {
    DEBUG ((DEBUG_ERROR, "Ramfb: memory allocation failed\n"));
    Status = EFI_OUT_OF_RESOURCES;
    goto FreeVgpuGop;
  }
  DEBUG ((DEBUG_INFO, "Ramfb: Framebuffer at 0x%lx, %lu kB, %lu pages\n",
    (UINT64)FbBase, (UINT64)(MaxFbSize / 1024), (UINT64)Pages));
  VgpuGop->GopMode.FrameBufferSize = MaxFbSize;
  VgpuGop->GopMode.FrameBufferBase = FbBase;
  VgpuGop->GopMode.MaxMode = ARRAY_SIZE (mQemuRamfbModeInfo);

  //
  // Format a human-readable controller name for VGPU_GOP, and stash it for
  // VirtioGpuGetControllerName() to look up. We simply append NameSuffix to
  // ParentBus->BusName.
  //
  Status = LookupUnicodeString2 ("en", mComponentName2.SupportedLanguages,
             ParentBus->BusName, &ParentBusName, FALSE /* Iso639Language */);
  ASSERT_EFI_ERROR (Status);
  NameSize = StrSize (ParentBusName) - sizeof (CHAR16) + sizeof NameSuffix;
  Name = AllocatePool (NameSize);
  if (Name == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreeFramebuffer;
  }
  UnicodeSPrintAsciiFormat (Name, NameSize, "%s%s", ParentBusName, NameSuffix);
  Status = AddUnicodeString2 ("en", mComponentName2.SupportedLanguages,
             &VgpuGop->GopName, Name, FALSE /* Iso639Language */);
  FreePool (Name);
  if (EFI_ERROR (Status)) {
    goto FreeFramebuffer;
  }

  //
  // Create the child device path.
  //
  VgpuGop->GopDevicePath = AppendDevicePathNode (ParentDevicePath,
                             &mAcpiAdr.Header);
  if (VgpuGop->GopDevicePath == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreeVgpuGopName;
  }

  //
  // Create the child handle with the child device path.
  //
  Status = gBS->InstallProtocolInterface (&VgpuGop->GopHandle,
                  &gEfiDevicePathProtocolGuid, EFI_NATIVE_INTERFACE,
                  VgpuGop->GopDevicePath);
  if (EFI_ERROR (Status)) {
    goto FreeDevicePath;
  }

  //
  // The child handle must present a reference to the parent handle's Virtio
  // Device Protocol interface.
  //
  Status = gBS->OpenProtocol (ParentBusController, &gVirtioDeviceProtocolGuid,
                  &ParentVirtIo, DriverBindingHandle, VgpuGop->GopHandle,
                  EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER);
  if (EFI_ERROR (Status)) {
    goto UninstallDevicePath;
  }
  ASSERT (ParentVirtIo == ParentBus->VirtIo);

  //
  // Initialize our Graphics Output Protocol.
  //
  // Fill in the function members of VgpuGop->Gop from the template, then set
  // up the rest of the GOP infrastructure by calling SetMode() right now.
  //
  CopyMem (&VgpuGop->Gop, &mQemuRamfbGraphicsOutput,
           sizeof mQemuRamfbGraphicsOutput);
  Status = VgpuGop->Gop.SetMode (&VgpuGop->Gop, 1);
  if (EFI_ERROR (Status)) {
    goto CloseVirtIoByChild;
  }

  //
  // Install the Graphics Output Protocol on the child handle.
  //
  Status = gBS->InstallProtocolInterface (&VgpuGop->GopHandle,
                  &gEfiGraphicsOutputProtocolGuid, EFI_NATIVE_INTERFACE,
                  &VgpuGop->Gop);
  if (EFI_ERROR (Status)) {
    goto CloseVirtIoByChild;
  }

  //
  // We're done.
  //
  ParentBus->Child = VgpuGop;
  return EFI_SUCCESS;

CloseVirtIoByChild:
  gBS->CloseProtocol (ParentBusController, &gVirtioDeviceProtocolGuid,
    DriverBindingHandle, VgpuGop->GopHandle);

UninstallDevicePath:
  gBS->UninstallProtocolInterface (VgpuGop->GopHandle,
         &gEfiDevicePathProtocolGuid, VgpuGop->GopDevicePath);
FreeDevicePath:
  FreePool (VgpuGop->GopDevicePath);
FreeVgpuGopName:
  FreeUnicodeStringTable (VgpuGop->GopName);
FreeFramebuffer:
  FreePages ((VOID*)(UINTN)VgpuGop->GopMode.FrameBufferBase, Pages);
FreeVgpuGop:
  FreePool (VgpuGop);
  return Status;
}

/**
  Dynamically allocate and initialize the VGPU_GOP child object within an
  otherwise configured parent VGPU_DEV object.

  This function adds a BY_CHILD_CONTROLLER reference to ParentBusController's
  VIRTIO_DEVICE_PROTOCOL interface.

  @param[in,out] ParentBus        The pre-initialized VGPU_DEV object that the
                                  newly created VGPU_GOP object will be the
                                  child of.

  @param[in] ParentDevicePath     The device path protocol instance that is
                                  installed on ParentBusController.

  @param[in] ParentBusController  The UEFI controller handle on which the
                                  ParentBus VGPU_DEV object and the
                                  ParentDevicePath device path protocol are
                                  installed.

  @param[in] DriverBindingHandle  The DriverBindingHandle member of
                                  EFI_DRIVER_BINDING_PROTOCOL whose Start()
                                  function is calling this function. It is
                                  passed as AgentHandle to gBS->OpenProtocol()
                                  when creating the BY_CHILD_CONTROLLER
                                  reference.

  @retval EFI_SUCCESS           ParentBus->Child has been created and
                                populated, and ParentBus->Child->GopHandle now
                                references ParentBusController->VirtIo
                                BY_CHILD_CONTROLLER.

  @retval EFI_OUT_OF_RESOURCES  Memory allocation failed.

  @return                       Error codes from underlying functions.
**/
STATIC
EFI_STATUS
InitVgpuGop (
  IN OUT VGPU_DEV                 *ParentBus,
  IN     EFI_DEVICE_PATH_PROTOCOL *ParentDevicePath,
  IN     EFI_HANDLE               ParentBusController,
  IN     EFI_HANDLE               DriverBindingHandle
  )
{
  VGPU_GOP            *VgpuGop;
  EFI_STATUS          Status;
  CHAR16              *ParentBusName;
  STATIC CONST CHAR16 NameSuffix[] = L" Head #0";
  UINTN               NameSize;
  CHAR16              *Name;
  EFI_TPL             OldTpl;
  VOID                *ParentVirtIo;

  VgpuGop = AllocateZeroPool (sizeof *VgpuGop);
  if (VgpuGop == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  VgpuGop->Signature = VGPU_GOP_SIG;
  VgpuGop->ParentBus = ParentBus;

  //
  // Format a human-readable controller name for VGPU_GOP, and stash it for
  // VirtioGpuGetControllerName() to look up. We simply append NameSuffix to
  // ParentBus->BusName.
  //
  Status = LookupUnicodeString2 ("en", mComponentName2.SupportedLanguages,
             ParentBus->BusName, &ParentBusName, FALSE /* Iso639Language */);
  ASSERT_EFI_ERROR (Status);
  NameSize = StrSize (ParentBusName) - sizeof (CHAR16) + sizeof NameSuffix;
  Name = AllocatePool (NameSize);
  if (Name == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreeVgpuGop;
  }
  UnicodeSPrintAsciiFormat (Name, NameSize, "%s%s", ParentBusName, NameSuffix);
  Status = AddUnicodeString2 ("en", mComponentName2.SupportedLanguages,
             &VgpuGop->GopName, Name, FALSE /* Iso639Language */);
  FreePool (Name);
  if (EFI_ERROR (Status)) {
    goto FreeVgpuGop;
  }

  //
  // Create the child device path.
  //
  VgpuGop->GopDevicePath = AppendDevicePathNode (ParentDevicePath,
                             &mAcpiAdr.Header);
  if (VgpuGop->GopDevicePath == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreeVgpuGopName;
  }

  //
  // Mask protocol notify callbacks until we're done.
  //
  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);

  //
  // Create the child handle with the child device path.
  //
  Status = gBS->InstallProtocolInterface (&VgpuGop->GopHandle,
                  &gEfiDevicePathProtocolGuid, EFI_NATIVE_INTERFACE,
                  VgpuGop->GopDevicePath);
  if (EFI_ERROR (Status)) {
    goto FreeDevicePath;
  }

  //
  // The child handle must present a reference to the parent handle's Virtio
  // Device Protocol interface.
  //
  Status = gBS->OpenProtocol (ParentBusController, &gVirtioDeviceProtocolGuid,
                  &ParentVirtIo, DriverBindingHandle, VgpuGop->GopHandle,
                  EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER);
  if (EFI_ERROR (Status)) {
    goto UninstallDevicePath;
  }
  ASSERT (ParentVirtIo == ParentBus->VirtIo);

  //
  // Initialize our Graphics Output Protocol.
  //
  // Fill in the function members of VgpuGop->Gop from the template, then set
  // up the rest of the GOP infrastructure by calling SetMode() right now.
  //
  CopyMem (&VgpuGop->Gop, &mGopTemplate, sizeof mGopTemplate);
  Status = VgpuGop->Gop.SetMode (&VgpuGop->Gop, 0);
  if (EFI_ERROR (Status)) {
    goto CloseVirtIoByChild;
  }

  //
  // Install the Graphics Output Protocol on the child handle.
  //
  Status = gBS->InstallProtocolInterface (&VgpuGop->GopHandle,
                  &gEfiGraphicsOutputProtocolGuid, EFI_NATIVE_INTERFACE,
                  &VgpuGop->Gop);
  if (EFI_ERROR (Status)) {
    goto UninitGop;
  }

  //
  // We're done.
  //
  gBS->RestoreTPL (OldTpl);
  ParentBus->Child = VgpuGop;
  return EFI_SUCCESS;

UninitGop:
  ReleaseGopResources (VgpuGop, TRUE /* DisableHead */);

CloseVirtIoByChild:
  gBS->CloseProtocol (ParentBusController, &gVirtioDeviceProtocolGuid,
    DriverBindingHandle, VgpuGop->GopHandle);

UninstallDevicePath:
  gBS->UninstallProtocolInterface (VgpuGop->GopHandle,
         &gEfiDevicePathProtocolGuid, VgpuGop->GopDevicePath);

FreeDevicePath:
  gBS->RestoreTPL (OldTpl);
  FreePool (VgpuGop->GopDevicePath);

FreeVgpuGopName:
  FreeUnicodeStringTable (VgpuGop->GopName);

FreeVgpuGop:
  FreePool (VgpuGop);

  return Status;
}

STATIC
VOID
UninitQemuRamfbGop (
  IN OUT VGPU_DEV   *ParentBus,
  IN     EFI_HANDLE ParentBusController,
  IN     EFI_HANDLE DriverBindingHandle
  )
{
  VGPU_GOP   *VgpuGop;
  UINTN      Pages;
  EFI_STATUS Status;

  ASSERT (!VgpuGop->UseRamfb);

  VgpuGop = ParentBus->Child;
  Status = gBS->UninstallProtocolInterface (VgpuGop->GopHandle,
                  &gEfiGraphicsOutputProtocolGuid, &VgpuGop->Gop);
  ASSERT_EFI_ERROR (Status);

  Status = gBS->CloseProtocol (ParentBusController, &gVirtioDeviceProtocolGuid,
                  DriverBindingHandle, VgpuGop->GopHandle);
  ASSERT_EFI_ERROR (Status);

  Status = gBS->UninstallProtocolInterface (VgpuGop->GopHandle,
                  &gEfiDevicePathProtocolGuid, VgpuGop->GopDevicePath);
  ASSERT_EFI_ERROR (Status);

  FreePool (VgpuGop->GopDevicePath);
  FreeUnicodeStringTable (VgpuGop->GopName);
  Pages = EFI_SIZE_TO_PAGES (VgpuGop->GopMode.FrameBufferSize);
  FreePages ((VOID*)(UINTN)VgpuGop->GopMode.FrameBufferBase, Pages);
  FreePool (VgpuGop);

  ParentBus->Child = NULL;

  // restore VGPU
  Status = VirtioGpuInit (ParentBus);
  ASSERT_EFI_ERROR (Status);

  Status = gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_CALLBACK,
                  VirtioGpuExitBoot, ParentBus /* NotifyContext */,
                  &ParentBus->ExitBoot);
  ASSERT_EFI_ERROR (Status);
}

/**
  Tear down and release the VGPU_GOP child object within the VGPU_DEV parent
  object.

  This function removes the BY_CHILD_CONTROLLER reference from
  ParentBusController's VIRTIO_DEVICE_PROTOCOL interface.

  @param[in,out] ParentBus        The VGPU_DEV object that the VGPU_GOP child
                                  object will be removed from.

  @param[in] ParentBusController  The UEFI controller handle on which the
                                  ParentBus VGPU_DEV object is installed.

  @param[in] DriverBindingHandle  The DriverBindingHandle member of
                                  EFI_DRIVER_BINDING_PROTOCOL whose Stop()
                                  function is calling this function. It is
                                  passed as AgentHandle to gBS->CloseProtocol()
                                  when removing the BY_CHILD_CONTROLLER
                                  reference.
**/
STATIC
VOID
UninitVgpuGop (
  IN OUT VGPU_DEV   *ParentBus,
  IN     EFI_HANDLE ParentBusController,
  IN     EFI_HANDLE DriverBindingHandle
  )
{
  VGPU_GOP   *VgpuGop;
  EFI_STATUS Status;

  VgpuGop = ParentBus->Child;
  Status = gBS->UninstallProtocolInterface (VgpuGop->GopHandle,
                  &gEfiGraphicsOutputProtocolGuid, &VgpuGop->Gop);
  ASSERT_EFI_ERROR (Status);

  //
  // Uninitialize VgpuGop->Gop.
  //
  ReleaseGopResources (VgpuGop, TRUE /* DisableHead */);

  Status = gBS->CloseProtocol (ParentBusController, &gVirtioDeviceProtocolGuid,
                  DriverBindingHandle, VgpuGop->GopHandle);
  ASSERT_EFI_ERROR (Status);

  Status = gBS->UninstallProtocolInterface (VgpuGop->GopHandle,
                  &gEfiDevicePathProtocolGuid, VgpuGop->GopDevicePath);
  ASSERT_EFI_ERROR (Status);

  FreePool (VgpuGop->GopDevicePath);
  FreeUnicodeStringTable (VgpuGop->GopName);
  FreePool (VgpuGop);

  ParentBus->Child = NULL;
}

//
// Driver Binding Protocol Implementation.
//
STATIC
EFI_STATUS
EFIAPI
VirtioGpuDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                  ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS             Status;
  VIRTIO_DEVICE_PROTOCOL *VirtIo;

  //
  // - If RemainingDevicePath is NULL: the caller is interested in creating all
  //   child handles.
  // - If RemainingDevicePath points to an end node: the caller is not
  //   interested in creating any child handle.
  // - Otherwise, the caller would like to create the one child handle
  //   specified in RemainingDevicePath. In this case we have to see if the
  //   requested device path is supportable.
  //
  if (RemainingDevicePath != NULL &&
      !IsDevicePathEnd (RemainingDevicePath) &&
      (DevicePathNodeLength (RemainingDevicePath) != sizeof mAcpiAdr ||
       CompareMem (RemainingDevicePath, &mAcpiAdr, sizeof mAcpiAdr) != 0)) {
    return EFI_UNSUPPORTED;
  }

  //
  // Open the Virtio Device Protocol interface on the controller, BY_DRIVER.
  //
  Status = gBS->OpenProtocol (ControllerHandle, &gVirtioDeviceProtocolGuid,
                  (VOID **)&VirtIo, This->DriverBindingHandle,
                  ControllerHandle, EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (EFI_ERROR (Status)) {
    //
    // If this fails, then by default we cannot support ControllerHandle. There
    // is one exception: we've already bound the device, have not produced any
    // GOP child controller, and now the caller wants us to produce the child
    // controller (either specifically or as part of "all children"). That's
    // allowed.
    //
    if (Status == EFI_ALREADY_STARTED) {
      EFI_STATUS Status2;
      VGPU_DEV   *VgpuDev;

      Status2 = gBS->OpenProtocol (ControllerHandle, &gEfiCallerIdGuid,
                       (VOID **)&VgpuDev, This->DriverBindingHandle,
                       ControllerHandle, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
      ASSERT_EFI_ERROR (Status2);

      if (VgpuDev->Child == NULL &&
          (RemainingDevicePath == NULL ||
           !IsDevicePathEnd (RemainingDevicePath))) {
        Status = EFI_SUCCESS;
      }
    }

    return Status;
  }

  //
  // First BY_DRIVER open; check the VirtIo revision and subsystem.
  //
  if (VirtIo->Revision < VIRTIO_SPEC_REVISION (1, 0, 0) ||
      VirtIo->SubSystemDeviceId != VIRTIO_SUBSYSTEM_GPU_DEVICE) {
    Status = EFI_UNSUPPORTED;
    goto CloseVirtIo;
  }

  //
  // We'll need the device path of the VirtIo device both for formatting
  // VGPU_DEV.BusName and for populating VGPU_GOP.GopDevicePath.
  //
  Status = gBS->OpenProtocol (ControllerHandle, &gEfiDevicePathProtocolGuid,
                  NULL, This->DriverBindingHandle, ControllerHandle,
                  EFI_OPEN_PROTOCOL_TEST_PROTOCOL);

CloseVirtIo:
  gBS->CloseProtocol (ControllerHandle, &gVirtioDeviceProtocolGuid,
    This->DriverBindingHandle, ControllerHandle);

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
VirtioGpuDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                  ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS               Status;
  VIRTIO_DEVICE_PROTOCOL   *VirtIo;
  BOOLEAN                  VirtIoBoundJustNow;
  VGPU_DEV                 *VgpuDev;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;

  //
  // Open the Virtio Device Protocol.
  //
  // The result of this operation, combined with the checks in
  // VirtioGpuDriverBindingSupported(), uniquely tells us whether we are
  // binding the VirtIo controller on this call (with or without creating child
  // controllers), or else we're *only* creating child controllers.
  //
  Status = gBS->OpenProtocol (ControllerHandle, &gVirtioDeviceProtocolGuid,
                  (VOID **)&VirtIo, This->DriverBindingHandle,
                  ControllerHandle, EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (EFI_ERROR (Status)) {
    //
    // The assertions below are based on the success of
    // VirtioGpuDriverBindingSupported(): we bound ControllerHandle earlier,
    // without producing child handles, and now we're producing the GOP child
    // handle only.
    //
    ASSERT (Status == EFI_ALREADY_STARTED);

    Status = gBS->OpenProtocol (ControllerHandle, &gEfiCallerIdGuid,
                    (VOID **)&VgpuDev, This->DriverBindingHandle,
                    ControllerHandle, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    ASSERT_EFI_ERROR (Status);

    ASSERT (VgpuDev->Child == NULL);
    ASSERT (
      RemainingDevicePath == NULL || !IsDevicePathEnd (RemainingDevicePath));

    VirtIoBoundJustNow = FALSE;
  } else {
    VirtIoBoundJustNow = TRUE;

    //
    // Allocate the private structure.
    //
    VgpuDev = AllocateZeroPool (sizeof *VgpuDev);
    if (VgpuDev == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto CloseVirtIo;
    }
    VgpuDev->VirtIo = VirtIo;
  }

  //
  // Grab the VirtIo controller's device path. This is necessary regardless of
  // VirtIoBoundJustNow.
  //
  Status = gBS->OpenProtocol (ControllerHandle, &gEfiDevicePathProtocolGuid,
                  (VOID **)&DevicePath, This->DriverBindingHandle,
                  ControllerHandle, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (EFI_ERROR (Status)) {
    goto FreeVgpuDev;
  }

  //
  // Create VGPU_DEV if we've bound the VirtIo controller right now (that is,
  // if we aren't *only* creating child handles).
  //
  if (VirtIoBoundJustNow) {
    CHAR16 *VgpuDevName;

    //
    // Format a human-readable controller name for VGPU_DEV, and stash it for
    // VirtioGpuGetControllerName() to look up.
    //
    Status = FormatVgpuDevName (ControllerHandle, This->DriverBindingHandle,
               DevicePath, &VgpuDevName);
    if (EFI_ERROR (Status)) {
      goto FreeVgpuDev;
    }
    Status = AddUnicodeString2 ("en", mComponentName2.SupportedLanguages,
               &VgpuDev->BusName, VgpuDevName, FALSE /* Iso639Language */);
    FreePool (VgpuDevName);
    if (EFI_ERROR (Status)) {
      goto FreeVgpuDev;
    }

    Status = VirtioGpuInit (VgpuDev);
    if (EFI_ERROR (Status)) {
      goto FreeVgpuDevBusName;
    }

    Status = gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_CALLBACK,
                    VirtioGpuExitBoot, VgpuDev /* NotifyContext */,
                    &VgpuDev->ExitBoot);
    if (EFI_ERROR (Status)) {
      goto UninitGpu;
    }

    //
    // Install the VGPU_DEV "protocol interface" on ControllerHandle.
    //
    Status = gBS->InstallProtocolInterface (&ControllerHandle,
                    &gEfiCallerIdGuid, EFI_NATIVE_INTERFACE, VgpuDev);
    if (EFI_ERROR (Status)) {
      goto CloseExitBoot;
    }

    if (RemainingDevicePath != NULL && IsDevicePathEnd (RemainingDevicePath)) {
      //
      // No child handle should be produced; we're done.
      //
      DEBUG ((DEBUG_INFO, "%a: bound VirtIo=%p without producing GOP\n",
        __FUNCTION__, (VOID *)VgpuDev->VirtIo));
      return EFI_SUCCESS;
    }
  }

  //
  // Below we'll produce our single child handle: the caller requested it
  // either specifically, or as part of all child handles.
  //
  ASSERT (VgpuDev->Child == NULL);
  ASSERT (
    RemainingDevicePath == NULL || !IsDevicePathEnd (RemainingDevicePath));

  Status = InitQemuRamfbGop (VgpuDev, DevicePath, ControllerHandle,
             This->DriverBindingHandle);
  if (!EFI_ERROR (Status)) {
    if (VirtIoBoundJustNow) {
      gBS->CloseEvent (VgpuDev->ExitBoot);
      VirtioGpuUninit (VgpuDev);
    }
    return EFI_SUCCESS;
  }

  Status = InitVgpuGop (VgpuDev, DevicePath, ControllerHandle,
             This->DriverBindingHandle);
  if (EFI_ERROR (Status)) {
    goto UninstallVgpuDev;
  }

  //
  // We're done.
  //
  DEBUG ((DEBUG_INFO, "%a: produced GOP %a VirtIo=%p\n", __FUNCTION__,
    VirtIoBoundJustNow ? "while binding" : "for pre-bound",
    (VOID *)VgpuDev->VirtIo));
  return EFI_SUCCESS;

UninstallVgpuDev:
  if (VirtIoBoundJustNow) {
    gBS->UninstallProtocolInterface (ControllerHandle, &gEfiCallerIdGuid,
           VgpuDev);
  }

CloseExitBoot:
  if (VirtIoBoundJustNow) {
    gBS->CloseEvent (VgpuDev->ExitBoot);
  }

UninitGpu:
  if (VirtIoBoundJustNow) {
    VirtioGpuUninit (VgpuDev);
  }

FreeVgpuDevBusName:
  if (VirtIoBoundJustNow) {
    FreeUnicodeStringTable (VgpuDev->BusName);
  }

FreeVgpuDev:
  if (VirtIoBoundJustNow) {
    FreePool (VgpuDev);
  }

CloseVirtIo:
  if (VirtIoBoundJustNow) {
    gBS->CloseProtocol (ControllerHandle, &gVirtioDeviceProtocolGuid,
      This->DriverBindingHandle, ControllerHandle);
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
VirtioGpuDriverBindingStop (
  IN  EFI_DRIVER_BINDING_PROTOCOL *This,
  IN  EFI_HANDLE                  ControllerHandle,
  IN  UINTN                       NumberOfChildren,
  IN  EFI_HANDLE                  *ChildHandleBuffer OPTIONAL
  )
{
  EFI_STATUS Status;
  VGPU_DEV   *VgpuDev;

  //
  // Look up the VGPU_DEV "protocol interface" on ControllerHandle.
  //
  Status = gBS->OpenProtocol (ControllerHandle, &gEfiCallerIdGuid,
                  (VOID **)&VgpuDev, This->DriverBindingHandle,
                  ControllerHandle, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  //
  // Sanity check: if we found gEfiCallerIdGuid on ControllerHandle, then we
  // keep its Virtio Device Protocol interface open BY_DRIVER.
  //
  ASSERT_EFI_ERROR (EfiTestManagedDevice (ControllerHandle,
                      This->DriverBindingHandle, &gVirtioDeviceProtocolGuid));

  switch (NumberOfChildren) {
  case 0:
    //
    // The caller wants us to unbind the VirtIo controller.
    //
    if (VgpuDev->Child != NULL) {
      //
      // We still have the GOP child.
      //
      Status = EFI_DEVICE_ERROR;
      break;
    }

    DEBUG ((DEBUG_INFO, "%a: unbinding GOP-less VirtIo=%p\n", __FUNCTION__,
      (VOID *)VgpuDev->VirtIo));

    Status = gBS->UninstallProtocolInterface (ControllerHandle,
                    &gEfiCallerIdGuid, VgpuDev);
    ASSERT_EFI_ERROR (Status);

    Status = gBS->CloseEvent (VgpuDev->ExitBoot);
    ASSERT_EFI_ERROR (Status);

    VirtioGpuUninit (VgpuDev);
    FreeUnicodeStringTable (VgpuDev->BusName);
    FreePool (VgpuDev);

    Status = gBS->CloseProtocol (ControllerHandle, &gVirtioDeviceProtocolGuid,
                    This->DriverBindingHandle, ControllerHandle);
    ASSERT_EFI_ERROR (Status);
    break;

  case 1:
    //
    // The caller wants us to destroy our child GOP controller.
    //
    if (VgpuDev->Child == NULL ||
        ChildHandleBuffer[0] != VgpuDev->Child->GopHandle) {
      //
      // We have no child controller at the moment, or it differs from the one
      // the caller wants us to destroy. I.e., we don't own the child
      // controller passed in.
      //
      Status = EFI_DEVICE_ERROR;
      break;
    }
    //
    // Sanity check: our GOP child controller keeps the VGPU_DEV controller's
    // Virtio Device Protocol interface open BY_CHILD_CONTROLLER.
    //
    ASSERT_EFI_ERROR (EfiTestChildHandle (ControllerHandle,
                        VgpuDev->Child->GopHandle,
                        &gVirtioDeviceProtocolGuid));

    DEBUG ((DEBUG_INFO, "%a: destroying GOP under VirtIo=%p\n", __FUNCTION__,
      (VOID *)VgpuDev->VirtIo));
    if (VgpuDev->Child->UseRamfb) {
      UninitQemuRamfbGop (VgpuDev, ControllerHandle, This->DriverBindingHandle);
    } else {
      UninitVgpuGop (VgpuDev, ControllerHandle, This->DriverBindingHandle);
    }
    break;

  default:
    //
    // Impossible, we never produced more than one child.
    //
    Status = EFI_DEVICE_ERROR;
    break;
  }
  return Status;
}

STATIC EFI_DRIVER_BINDING_PROTOCOL mDriverBinding = {
  VirtioGpuDriverBindingSupported,
  VirtioGpuDriverBindingStart,
  VirtioGpuDriverBindingStop,
  0x10,                            // Version
  NULL,                            // ImageHandle, overwritten in entry point
  NULL                             // DriverBindingHandle, ditto
};

//
// Entry point of the driver.
//
EFI_STATUS
EFIAPI
VirtioGpuEntryPoint (
  IN EFI_HANDLE       ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  return EfiLibInstallDriverBindingComponentName2 (ImageHandle, SystemTable,
           &mDriverBinding, ImageHandle, NULL /* ComponentName */,
           &mComponentName2);
}
