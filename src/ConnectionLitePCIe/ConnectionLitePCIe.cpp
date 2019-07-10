#include "ConnectionLitePCIe.h"
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
#include "Logger.h"
#include "dataTypes.h"

using namespace std;

using namespace lime;

static const char* ep_names[] = { "/dev/litepcie1",  "/dev/litepcie2",  "/dev/litepcie3"};

ConnectionLitePCIe::ConnectionLitePCIe(const unsigned) :
    isConnected(true)
{
    control_fd = open(LITEPCIE_FILENAME, O_RDWR);
    if (control_fd<0)
    {
        isConnected = false;
        lime::error("Failed to open Lite PCIe");
        return;
    }
    for (int i = 0; i < MAX_EP_CNT; i++)
    {
        ep_fd[i] = open(ep_names[i], O_RDWR);
        rxDMAstarted[i] = txDMAstarted[i] = false;
    }
}

ConnectionLitePCIe::~ConnectionLitePCIe()
{
    if (control_fd >=0)
        close(control_fd);
}

bool ConnectionLitePCIe::IsOpen()
{
    return isConnected;
}

int ConnectionLitePCIe::Write(const unsigned char *buffer, const int length, int /*timeout_ms*/)
{
    return write(control_fd, buffer, length);
}

int ConnectionLitePCIe::Read(unsigned char *buffer, const int length, int timeout_ms)
{
    uint32_t status = 0;
    auto t1 = chrono::high_resolution_clock::now();
    do
    {   //wait for status byte to change
        read(control_fd, &status, sizeof(status));
        if ((status&0xFF00) != 0)
            break;
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    }
    while (std::chrono::duration_cast<std::chrono::milliseconds>(chrono::high_resolution_clock::now() - t1).count() < timeout_ms);

    if ((status&0xFF00)== 0)
        return 0;   //timeout
    return read(control_fd, buffer, length);
}

int ConnectionLitePCIe::GetBuffersCount() const
{
    return 1;
}

int ConnectionLitePCIe::CheckStreamSize(int size) const
{
    return size;
}

int ConnectionLitePCIe::ResetStreamBuffers()
{
    for (int i = 0; i < MAX_EP_CNT; i++)
    {
        if (txDMAstarted[i].load(std::memory_order_relaxed))
            litepcie_dma_stop(control_fd, i, DMA_CHANNEL_TX);
        if (rxDMAstarted[i].load(std::memory_order_relaxed))
            litepcie_dma_stop(control_fd, i, DMA_CHANNEL_RX);
        rxDMAstarted[i].store(false, std::memory_order_relaxed);
        txDMAstarted[i].store(false, std::memory_order_relaxed);
    }
    return 0;
}

int ConnectionLitePCIe::ReceiveData(char *buffer, int length, int epIndex, int timeout_ms)
{
    if (!rxDMAstarted[epIndex].load(std::memory_order_relaxed))
    {
        unsigned size = length/sizeof(FPGA_DataPacket);
        size = size > 16 ? 16 : size ? size : 1;
        litepcie_dma_start(control_fd, size*sizeof(FPGA_DataPacket), epIndex, DMA_CHANNEL_RX);
        rxDMAstarted[epIndex].store(true, std::memory_order_relaxed);
    }
    int totalBytesReaded = 0;
    int bytesToRead = length;
    auto t1 = chrono::high_resolution_clock::now();

    do
    {
        int bytesReceived = read(ep_fd[epIndex], buffer+totalBytesReaded, length-totalBytesReaded);
        if (bytesReceived == 0)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        totalBytesReaded += bytesReceived;
        if (totalBytesReaded < length)
            bytesToRead -= bytesReceived;
        else
            break;
    }while (std::chrono::duration_cast<std::chrono::milliseconds>(chrono::high_resolution_clock::now() - t1).count() < timeout_ms);

    return totalBytesReaded;
}

void ConnectionLitePCIe::AbortReading(int epIndex)
{
    if (rxDMAstarted[epIndex].load(std::memory_order_relaxed))
    {
        rxDMAstarted[epIndex].store(false, std::memory_order_relaxed);
        litepcie_dma_stop(control_fd, epIndex, DMA_CHANNEL_RX);
    }
}

int ConnectionLitePCIe::SendData(const char *buffer, int length, int epIndex, int timeout_ms)
{
    if (!txDMAstarted[epIndex].load(std::memory_order_relaxed))
    {
        unsigned size = length/sizeof(FPGA_DataPacket);
        size = size > 16 ? 16 : size ? size : 1;
        litepcie_dma_start(control_fd, size*sizeof(FPGA_DataPacket), epIndex, DMA_CHANNEL_TX);
        txDMAstarted[epIndex].store(true, std::memory_order_relaxed);
    }

    int totalBytesSent = 0;
    int bytesToSend = length;
    auto t1 = chrono::high_resolution_clock::now();
    do
    {
        int bytesSent = write(ep_fd[epIndex], buffer+totalBytesSent, length-totalBytesSent);
        if (bytesSent == 0)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }
        totalBytesSent += bytesSent;
        if (totalBytesSent < length)
            bytesToSend -= bytesSent;
        else
            break;
    }while (std::chrono::duration_cast<std::chrono::milliseconds>(chrono::high_resolution_clock::now() - t1).count() < timeout_ms);
    return totalBytesSent;
}

void ConnectionLitePCIe::AbortSending(int epIndex)
{
    if (txDMAstarted[epIndex].load(std::memory_order_relaxed))
    {
        txDMAstarted[epIndex].store(false, std::memory_order_relaxed);
        litepcie_dma_stop(control_fd, epIndex, DMA_CHANNEL_TX);
    }
}

int ConnectionLitePCIe::BeginDataReading(char* buffer, uint32_t length, int ep)
{
    return ep;
}

bool ConnectionLitePCIe::WaitForReading(int contextHandle, unsigned int timeout_ms)
{
    return true;
}

int ConnectionLitePCIe::FinishDataReading(char* buffer, uint32_t length, int contextHandle)
{
    return ReceiveData(buffer, length, contextHandle, 3000);
}

int ConnectionLitePCIe::BeginDataSending(const char* buffer, uint32_t length, int ep)
{
    return SendData(buffer, length,  ep, 3000);
}

bool ConnectionLitePCIe::WaitForSending(int contextHandle, uint32_t timeout_ms)
{
    return true;
}

int ConnectionLitePCIe::FinishDataSending(const char* buffer, uint32_t length, int contextHandle)
{
    return contextHandle;
}