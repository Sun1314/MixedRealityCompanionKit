#include "pch.h"
#include "Logger.h"

#include <locale>
#include <codecvt> 

using namespace Microsoft::WRL;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Storage;
using namespace ABI::Windows::Storage::Streams;

std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t> WCharConverter; // converter object saved somewhere

class SafeString
{
public:
    SafeString() throw() : m_hstring(nullptr) { }
    ~SafeString() throw() { if (nullptr != m_hstring) { WindowsDeleteString(m_hstring); } }
    operator const HSTRING&() const { return m_hstring; }
    HSTRING* GetAddressOf() { return &m_hstring; }
    const wchar_t* c_str() const { return WindowsGetStringRawBuffer(m_hstring, nullptr); }
private:
    HSTRING m_hstring;
};

ComPtr<ILogger> LoggerImpl::s_spInstance = nullptr;

ILogger* LoggerImpl::Instance()
{
    if (nullptr == s_spInstance)
    {
        ComPtr<ILogger> spLogger;
        if (SUCCEEDED(MakeAndInitialize<LoggerImpl>(&spLogger, L"App_Log.txt")))
        {
            s_spInstance.Attach(spLogger.Detach());
        }
    }

    return s_spInstance.Get();
}

LoggerImpl::LoggerImpl()
    : m_savingToFile(false)
    , m_spOutputStream(nullptr)
    , m_spDataWriter(nullptr)
{
}

LoggerImpl::~LoggerImpl()
{
}

HRESULT LoggerImpl::RuntimeClassInitialize(LPCWSTR filename)
{
    // create temp folder
    ComPtr<IApplicationDataStatics> applicationStatics;
    ComPtr<IApplicationData> applicationData;
    ComPtr<IStorageFolder> tempFolder;
    ComPtr<IStorageItem> tempStorageItem;
    SafeString path;
    SafeString hstrFilename;
    ComPtr<IAsyncOperation<StorageFile*>> createFileOp;

    HRESULT hr = Windows::Foundation::GetActivationFactory(
        Wrappers::HStringReference(RuntimeClass_Windows_Storage_ApplicationData).Get(),
        &applicationStatics);

    if (SUCCEEDED(hr))
        hr = applicationStatics->get_Current(&applicationData);

    if (SUCCEEDED(hr))
        hr = applicationData->get_TemporaryFolder(&tempFolder);

    if (SUCCEEDED(hr))
        hr = tempFolder.As(&tempStorageItem);

    if (SUCCEEDED(hr))
        hr = tempStorageItem->get_Path(path.GetAddressOf());

    if (SUCCEEDED(hr))
        hr = WindowsCreateString(filename, static_cast<UINT32>(wcslen(filename)), hstrFilename.GetAddressOf());

    if (SUCCEEDED(hr))
        hr = tempFolder->CreateFileAsync(hstrFilename, ABI::Windows::Storage::CreationCollisionOption_ReplaceExisting, &createFileOp);

    auto fileCreatedHandler = Callback<IAsyncOperationCompletedHandler<StorageFile*>>(this, &LoggerImpl::OnFileCreated);
    if (SUCCEEDED(hr))
        hr = createFileOp->put_Completed(fileCreatedHandler.Get());

    return hr;
}

HRESULT LoggerImpl::OnFileCreated(IAsyncOperation<StorageFile*>* asyncOp, AsyncStatus status)
{
    assert(status == AsyncStatus::Completed);

    ComPtr<IStorageFile> createdFile;
    ComPtr<IAsyncOperation<IRandomAccessStream*>> openOp;

    HRESULT hr = asyncOp->GetResults(&createdFile);
    
    if (SUCCEEDED(hr))
        hr = createdFile->OpenAsync(FileAccessMode_ReadWrite, &openOp);

    auto fileOpenedHandler = Callback<IAsyncOperationCompletedHandler<IRandomAccessStream*>>(this, &LoggerImpl::OnFileOpened);
    
    if (SUCCEEDED(hr))
        hr = openOp->put_Completed(fileOpenedHandler.Get());

    return hr;
}

HRESULT LoggerImpl::OnFileOpened(IAsyncOperation<IRandomAccessStream*>* asyncOp, AsyncStatus status)
{
    assert(status == AsyncStatus::Completed);

    ComPtr<IRandomAccessStream> randomAccesssStream;
    ComPtr<IOutputStream> outputStream;
    ComPtr<IDataWriterFactory> dataWriterFactory;
    ComPtr<IDataWriter> dataWriter;

    HRESULT hr = asyncOp->GetResults(&randomAccesssStream);
    if (SUCCEEDED(hr))
        hr = randomAccesssStream->GetOutputStreamAt(0, &outputStream);

    if (SUCCEEDED(hr))
        hr = Windows::Foundation::GetActivationFactory(
            Wrappers::HStringReference(RuntimeClass_Windows_Storage_Streams_DataWriter).Get(),
            &dataWriterFactory);

    if (SUCCEEDED(hr))
        hr = dataWriterFactory->CreateDataWriter(outputStream.Get(), &dataWriter);

    if (FAILED(hr))
        return hr;

    auto lock = _lock.Lock();

    m_spDataWriter.Attach(dataWriter.Detach());
    m_spOutputStream.Attach(outputStream.Detach());

    return FlushMessageQueue();
}

HRESULT LoggerImpl::Log(HSTRING message)
{
    if (nullptr == message)
        return E_INVALIDARG;

    auto lock = _lock.Lock();

    const wchar_t* messageBuffer = WindowsGetStringRawBuffer(message, nullptr);
    m_logMessages.push_back(messageBuffer);

    if (m_spDataWriter == nullptr || m_savingToFile)
    {
        return S_OK;
    }

    return FlushMessageQueue();
}

HRESULT LoggerImpl::FlushMessageQueue()
{
    std::vector<std::wstring> messages;
    messages.swap(m_logMessages);
    if (messages.size() == 0)
    {
        m_savingToFile = false;

        return S_OK;
    }

    HRESULT hr = S_OK;
    UINT32 length = 0;
    for each (auto& message in messages)
    {
        std::string output = WCharConverter.to_bytes(message);
        hr = m_spDataWriter->WriteBytes(static_cast<UINT32>(output.length()), const_cast<BYTE*>(reinterpret_cast<const BYTE*>(output.c_str())));
        if (SUCCEEDED(hr))
            length += static_cast<UINT32>(output.length());
    }

    if (FAILED(hr))
        return hr;

    ComPtr<IAsyncOperation<UINT32>> writeToStoreOp;
    if (SUCCEEDED(hr))
        hr = m_spDataWriter->StoreAsync(&writeToStoreOp);

    if (FAILED(hr))
        return hr;

    auto writeToStoreHandler = Callback<IAsyncOperationCompletedHandler<UINT32>>(
        [this, length](IAsyncOperation<UINT32>* asyncOp, AsyncStatus status) -> HRESULT
    {
        assert(status == AsyncStatus::Completed);

        UINT32 bytesWritten = 0;
        if (SUCCEEDED(asyncOp->GetResults(&bytesWritten)))
        {
            assert(bytesWritten == length);
        
            auto cbFlush = Callback<IAsyncOperationCompletedHandler<bool>>(
                [this, length](IAsyncOperation<bool>* asyncOp, AsyncStatus status) -> HRESULT
            {
                assert(status == AsyncStatus::Completed);

                auto lock = _lock.Lock();

                return FlushMessageQueue();
            });

            ComPtr<IAsyncOperation<bool>> spFlushOp;
            HRESULT hr = m_spOutputStream->FlushAsync(&spFlushOp);
            if (SUCCEEDED(hr))
            {
                hr = spFlushOp->put_Completed(cbFlush.Get());
            }
        }

        return S_OK;
    });

    m_savingToFile = true;

    return writeToStoreOp->put_Completed(writeToStoreHandler.Get());
}
