/**********************************************************************
* Copyright (c) 2013-2014  Red Hat, Inc.
*
* Developed by Daynix Computing LTD.
*
* Authors:
*     Dmitry Fleytman <dmitry@daynix.com>
*     Pavel Gurvich <pavel@daynix.com>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
**********************************************************************/

#include "UsbTarget.h"
#include "Trace.h"
#include "UsbTarget.tmh"
#include "Usbdlib.h"
#include "DeviceAccess.h"
#include "WdfRequest.h"

class CWdfUrb final
{
public:
    CWdfUrb(WDFUSBDEVICE TargetDevice)
        : m_TargetDevice(TargetDevice)
    {}
    ~CWdfUrb();

    NTSTATUS Create();
    NTSTATUS CWdfUrb::SendSynchronously();

    template <typename TBuffer>
    void BuildDescriptorRequest(UCHAR Type, UCHAR Index, TBuffer &Buffer, ULONG BufferLength = sizeof(TBuffer));

private:
    WDFUSBDEVICE m_TargetDevice;
    PURB m_Urb = nullptr;
    WDFMEMORY m_UrbBuff = WDF_NO_HANDLE;

    CWdfUrb(const CWdfUrb&) = delete;
    CWdfUrb& operator= (const CWdfUrb&) = delete;
};

CWdfUrb::~CWdfUrb()
{
    if (m_UrbBuff != WDF_NO_HANDLE)
    {
        WdfObjectDelete(m_UrbBuff);
    }
}

NTSTATUS CWdfUrb::Create()
{
    auto status = WdfUsbTargetDeviceCreateUrb(m_TargetDevice, nullptr, &m_UrbBuff, &m_Urb);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Cannot create URB, %!STATUS!", status);
    }
    return status;
}

template <typename TBuffer>
void CWdfUrb::BuildDescriptorRequest(UCHAR Type, UCHAR Index, TBuffer &Buffer, ULONG BufferLength)
{
    UsbDkBuildDescriptorRequest(*m_Urb, Type, Index, Buffer, BufferLength);
}

NTSTATUS CWdfUrb::SendSynchronously()
{
    PAGED_CODE();

    auto status = WdfUsbTargetDeviceSendUrbSynchronously(m_TargetDevice, nullptr, nullptr, m_Urb);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Cannot send URB, %!STATUS!", status);
    }
    return status;
}

class CWdfUsbPipe : public CAllocatable<NonPagedPool, 'PUHR'>
{
public:
    CWdfUsbPipe()
    {}

    void Create(WDFUSBINTERFACE Interface, UCHAR PipeIndex);
    void ReadAsync(CWdfRequest &Request, WDFMEMORY Buffer, PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion);
    void WriteAsync(CWdfRequest &Request, WDFMEMORY Buffer, PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion);
    NTSTATUS Abort(WDFREQUEST Request);
    UCHAR EndpointAddress() const
    { return m_Info.EndpointAddress; }

private:
    WDFUSBINTERFACE m_Interface;
    WDFUSBPIPE m_Pipe;
    WDF_USB_PIPE_INFORMATION m_Info;

    CWdfUsbPipe(const CWdfUsbPipe&) = delete;
    CWdfUsbPipe& operator= (const CWdfUsbPipe&) = delete;
};

class CWdfUsbInterface : public CAllocatable<NonPagedPool, 'IUHR'>
{
public:
    CWdfUsbInterface()
        : m_Pipes(nullptr, CObjHolder<CWdfUsbPipe>::ArrayHolderDelete)
    {}

    NTSTATUS Create(WDFUSBDEVICE Device, UCHAR InterfaceIdx);
    NTSTATUS SetAltSetting(UCHAR AltSettingIdx);

    CWdfUsbPipe *FindPipeByEndpointAddress(ULONG64 EndpointAddress);

private:
    WDFUSBDEVICE m_UsbDevice;
    WDFUSBINTERFACE m_Interface;

    CObjHolder<CWdfUsbPipe> m_Pipes;
    BYTE m_NumPipes = 0;

    CWdfUsbInterface(const CWdfUsbInterface&) = delete;
    CWdfUsbInterface& operator= (const CWdfUsbInterface&) = delete;
};

NTSTATUS CWdfUsbInterface::SetAltSetting(UCHAR AltSettingIdx)
{
    WDF_USB_INTERFACE_SELECT_SETTING_PARAMS params;
    WDF_USB_INTERFACE_SELECT_SETTING_PARAMS_INIT_SETTING(&params, AltSettingIdx);

    auto status = WdfUsbInterfaceSelectSetting(m_Interface, WDF_NO_OBJECT_ATTRIBUTES, &params);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Failed: %!STATUS!", status);
        return status;
    }

    m_NumPipes = WdfUsbInterfaceGetNumConfiguredPipes(m_Interface);
    if (m_NumPipes == 0)
    {
        return STATUS_SUCCESS;
    }

    m_Pipes.destroy();
    m_Pipes = new CWdfUsbPipe[m_NumPipes];
    if (!m_Pipes)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Failed to allocate pipes array");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (UCHAR i = 0; i < m_NumPipes; i++)
    {
        m_Pipes[i].Create(m_Interface, i);
    }

    return STATUS_SUCCESS;
}

CWdfUsbPipe *CWdfUsbInterface::FindPipeByEndpointAddress(ULONG64 EndpointAddress)
{
    for (UCHAR i = 0; i < m_NumPipes; i++)
    {
        if (m_Pipes[i].EndpointAddress() == EndpointAddress)
        {
            return &m_Pipes[i];
        }
    }

    return nullptr;
}

NTSTATUS CWdfUsbInterface::Create(WDFUSBDEVICE Device, UCHAR InterfaceIdx)
{
    m_UsbDevice = Device;
    m_Interface = WdfUsbTargetDeviceGetInterface(Device, InterfaceIdx);
    ASSERT(m_Interface != nullptr);

    return SetAltSetting(0);
}

void CWdfUsbPipe::Create(WDFUSBINTERFACE Interface, UCHAR PipeIndex)
{
    m_Interface = Interface;

    WDF_USB_PIPE_INFORMATION_INIT(&m_Info);

    m_Pipe = WdfUsbInterfaceGetConfiguredPipe(m_Interface, PipeIndex, &m_Info);
    ASSERT(m_Pipe != nullptr);
    WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(m_Pipe);
}

void CWdfUsbPipe::ReadAsync(CWdfRequest &Request, WDFMEMORY Buffer, PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion)
{
    auto status = WdfUsbTargetPipeFormatRequestForRead(m_Pipe, Request, Buffer, nullptr);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! WdfUsbTargetPipeFormatRequestForRead failed: %!STATUS!", status);
        Request.SetStatus(status);
    }
    else
    {
        status = Request.SendWithCompletion(WdfUsbTargetPipeGetIoTarget(m_Pipe), Completion);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! send failed: %!STATUS!", status);
        }
    }
}

void CWdfUsbPipe::WriteAsync(CWdfRequest &Request, WDFMEMORY Buffer, PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion)
{
    auto status = WdfUsbTargetPipeFormatRequestForWrite(m_Pipe, Request, Buffer, nullptr);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! WdfUsbTargetPipeFormatRequestForWrite failed: %!STATUS!", status);
        Request.SetStatus(status);
    }
    else
    {
        status = Request.SendWithCompletion(WdfUsbTargetPipeGetIoTarget(m_Pipe), Completion);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! send failed: %!STATUS!", status);
        }
    }
}

NTSTATUS CWdfUsbPipe::Abort(WDFREQUEST Request)
{
    auto status = WdfUsbTargetPipeAbortSynchronously(m_Pipe, Request, nullptr);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! WdfUsbTargetPipeAbortSynchronously failed: %!STATUS!", status);
    }

    return status;
}

NTSTATUS CWdfUsbTarget::Create(WDFDEVICE Device)
{
    m_Device = Device;

    WDF_USB_DEVICE_CREATE_CONFIG Config;
    WDF_USB_DEVICE_CREATE_CONFIG_INIT(&Config, USBD_CLIENT_CONTRACT_VERSION_602);

    auto status = WdfUsbTargetDeviceCreateWithParameters(m_Device,
                                                         &Config,
                                                         WDF_NO_OBJECT_ATTRIBUTES,
                                                         &m_UsbDevice);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Cannot create USB target, %!STATUS!", status);
        return status;
    }

    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS Params;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_MULTIPLE_INTERFACES(&Params, 0, nullptr);
    status = WdfUsbTargetDeviceSelectConfig(m_UsbDevice, WDF_NO_OBJECT_ATTRIBUTES, &Params);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Cannot apply device configuration, %!STATUS!", status);
        return status;
    }

    m_NumInterfaces = WdfUsbTargetDeviceGetNumInterfaces(m_UsbDevice);
    if (m_NumInterfaces == 0)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Failed: Number of interfaces is zero.");
        return STATUS_INVALID_DEVICE_STATE;
    }

    m_Interfaces = new CWdfUsbInterface[m_NumInterfaces];
    if (!m_Interfaces)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Failed to allocate interfaces array");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (UCHAR i = 0; i < m_NumInterfaces; i++)
    {
        status = m_Interfaces[i].Create(m_UsbDevice, i);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Cannot create interface %d, %!STATUS!", i, status);
            return status;
        }
    }

    return STATUS_SUCCESS;
}

CWdfUsbTarget::CWdfUsbTarget()
    : m_Interfaces(nullptr, CObjHolder<CWdfUsbInterface>::ArrayHolderDelete)
{}

CWdfUsbTarget::~CWdfUsbTarget()
{
    if (m_UsbDevice != WDF_NO_HANDLE)
    {
        WdfObjectDelete(m_UsbDevice);
    }
}

void CWdfUsbTarget::DeviceDescriptor(USB_DEVICE_DESCRIPTOR &Descriptor)
{
    WdfUsbTargetDeviceGetDeviceDescriptor(m_UsbDevice, &Descriptor);
}

NTSTATUS CWdfUsbTarget::ConfigurationDescriptor(UCHAR Index, PUSB_CONFIGURATION_DESCRIPTOR Descriptor, PULONG TotalLength)
{
    CWdfUrb Urb(m_UsbDevice);
    auto status = Urb.Create();
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    // 1. Query descriptor total length
    USB_CONFIGURATION_DESCRIPTOR ShortDescriptor = {};
    Urb.BuildDescriptorRequest(USB_CONFIGURATION_DESCRIPTOR_TYPE, Index, ShortDescriptor);

    status = Urb.SendSynchronously();
    if (ShortDescriptor.wTotalLength == 0)
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Cannot query configuration descriptor length, %!STATUS!", status);
        if (NT_SUCCESS(status))
        {
            status = USBD_STATUS_INAVLID_CONFIGURATION_DESCRIPTOR;
        }
        return status;
    }

    // 2. Check buffer is big enough
    if (ShortDescriptor.wTotalLength > *TotalLength)
    {
        *TotalLength = ShortDescriptor.wTotalLength;
        return STATUS_BUFFER_OVERFLOW;
    }

    // 3. Query full descriptor
    RtlZeroMemory(Descriptor, *TotalLength);
    *TotalLength = ShortDescriptor.wTotalLength;

    Urb.BuildDescriptorRequest(USB_CONFIGURATION_DESCRIPTOR_TYPE, Index, *Descriptor, ShortDescriptor.wTotalLength);

    status = Urb.SendSynchronously();
    if ((Descriptor->wTotalLength == 0) || !NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Cannot query configuration descriptor, %!STATUS!", status);
        if (NT_SUCCESS(status))
        {
            status = USBD_STATUS_INAVLID_CONFIGURATION_DESCRIPTOR;
        }
    }
    return status;
}

NTSTATUS CWdfUsbTarget::SetInterfaceAltSetting(UCHAR InterfaceIdx, UCHAR AltSettingIdx)
{
    if (InterfaceIdx >= m_NumInterfaces)
    {
        return STATUS_INVALID_PARAMETER_1;
    }

    //TODO: Stop read/write queue before interface reconfiguration
    return m_Interfaces[InterfaceIdx].SetAltSetting(AltSettingIdx);
}

CWdfUsbPipe *CWdfUsbTarget::FindPipeByEndpointAddress(ULONG64 EndpointAddress)
{
    CWdfUsbPipe *Pipe = nullptr;

    for (UCHAR i = 0; i < m_NumInterfaces; i++)
    {
        Pipe = m_Interfaces[i].FindPipeByEndpointAddress(EndpointAddress);
        if (Pipe != nullptr)
        {
            break;
        }
    }

    return Pipe;
}

void CWdfUsbTarget::WritePipeAsync(WDFREQUEST Request, ULONG64 EndpointAddress, WDFMEMORY Buffer, PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion)
{
    CWdfRequest WdfRequest(Request);

    CWdfUsbPipe *Pipe = FindPipeByEndpointAddress(EndpointAddress);
    if (Pipe != nullptr)
    {
        Pipe->WriteAsync(WdfRequest, Buffer, Completion);
    }
    else
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Failed: Pipe not found");
        WdfRequest.SetStatus(STATUS_NOT_FOUND);
    }
}

void CWdfUsbTarget::ReadPipeAsync(WDFREQUEST Request, ULONG64 EndpointAddress, WDFMEMORY Buffer, PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion)
{
    CWdfRequest WdfRequest(Request);

    CWdfUsbPipe *Pipe = FindPipeByEndpointAddress(EndpointAddress);
    if (Pipe != nullptr)
    {
        Pipe->ReadAsync(WdfRequest, Buffer, Completion);
    }
    else
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Failed: Pipe not found");
        WdfRequest.SetStatus(STATUS_NOT_FOUND);
    }
}

NTSTATUS CWdfUsbTarget::AbortPipe(WDFREQUEST Request, ULONG64 EndpointAddress)
{
    auto Pipe = FindPipeByEndpointAddress(EndpointAddress);
    if (Pipe != nullptr)
    {
        return Pipe->Abort(Request);
    }
    else
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! Failed: Pipe not found");
        return STATUS_NOT_FOUND;
    }
}

NTSTATUS CWdfUsbTarget::ControlTransferAsync(CWdfRequest &WdfRequest, PWDF_USB_CONTROL_SETUP_PACKET SetupPacket, WDFMEMORY Data,
                                         PWDFMEMORY_OFFSET TransferOffset, PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion)
{
    auto status = WdfUsbTargetDeviceFormatRequestForControlTransfer(m_UsbDevice, WdfRequest, SetupPacket, Data, TransferOffset);

    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! WdfUsbTargetDeviceFormatRequestForControlTransfer failed: %!STATUS!", status);
    }
    else
    {
        status = WdfRequest.SendWithCompletion(WdfUsbTargetDeviceGetIoTarget(m_UsbDevice), Completion);
        if (!NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_USBTARGET, "%!FUNC! send failed: %!STATUS!", status);
        }
        else
        {
            WdfRequest.Detach();
        }
    }

    return status;
}
