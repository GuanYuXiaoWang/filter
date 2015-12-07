#include "log.h"
#include "list_entry.h"

typedef struct _LOG_ENTRY {
    CHAR Buf[256];
    ULONG BufUsed;
    LIST_ENTRY ListEntry;
} LOG_ENTRY, *PLOG_ENTRY;

typedef struct _LOG_CONTEXT {
    HANDLE hThread;
    HANDLE hFile;
    HANDLE hEvent;
    WCHAR  FilePath[256];
    LIST_ENTRY ListHead;
    CRITICAL_SECTION Lock;
    volatile LONG Stopping;
} LOG_CONTEXT, *PLOG_CONTEXT;

LOG_CONTEXT g_LogCtx;

LONG WriteMsg2(PCHAR *pBuff, ULONG *pLeft, PCHAR Fmt, va_list Args)
{
    LONG Result;

    if (*pLeft < 0)
        return -1;

    Result = _vsnprintf(*pBuff, *pLeft, Fmt, Args);
    if (Result) {
        *pBuff+=Result;
        *pLeft-=Result;
    } 

    return Result;
}

LONG WriteMsg(PCHAR *pBuff, ULONG *pLeft, PCHAR Fmt, ...)
{
    LONG Result;
    va_list Args;

    va_start(Args, Fmt);
    Result = WriteMsg2(pBuff, pLeft, Fmt, Args);
    va_end(Args);

    return Result;
}

PCHAR TruncatePath(PCHAR FileName)
{
    PCHAR BaseName;

    BaseName = strrchr(FileName, '\\');
    if (BaseName)
        return ++BaseName;
    else
        return FileName;
}

DWORD LogWriteEntry(PLOG_CONTEXT LogCtx, PLOG_ENTRY LogEntry)
{
    DWORD WrittenTotal = 0;
    DWORD Written;

    while (WrittenTotal < LogEntry->BufUsed) {
        if (!WriteFile(LogCtx->hFile, LogEntry->Buf + WrittenTotal,
                       LogEntry->BufUsed - WrittenTotal, &Written, NULL)) {
            return GetLastError();
            break;
        }
        WrittenTotal+= Written;
    }

    return 0;
}

DWORD LogThreadRoutine(PLOG_CONTEXT LogCtx)
{
    LIST_ENTRY LogEntries;
    PLIST_ENTRY ListEntry;
    PLOG_ENTRY LogEntry;

    while (!LogCtx->Stopping) {
        WaitForSingleObject(LogCtx->hEvent, INFINITE);
        if (LogCtx->Stopping)
            break;

        EnterCriticalSection(&LogCtx->Lock);
        if (LogCtx->Stopping) {
            LeaveCriticalSection(&LogCtx->Lock);
            break;
        }

        InitializeListHead(&LogEntries);
        while (!IsListEmpty(&LogCtx->ListHead)) {
            ListEntry = RemoveHeadList(&LogCtx->ListHead);
            InsertTailList(&LogEntries, ListEntry);
        }
        LeaveCriticalSection(&LogCtx->Lock);

        while (!IsListEmpty(&LogEntries)) {
            ListEntry = RemoveHeadList(&LogEntries);
            LogEntry = CONTAINING_RECORD(ListEntry, LOG_ENTRY, ListEntry);
            LogWriteEntry(LogCtx, LogEntry);
            free(LogEntry);
        }
        FlushFileBuffers(LogCtx->hFile);
    }

    return 0;
}

DWORD LogInit(PLOG_CONTEXT LogCtx, PWCHAR FilePath)
{
    memset(LogCtx, 0, sizeof(*LogCtx));

    InitializeListHead(&LogCtx->ListHead);
    _snwprintf(LogCtx->FilePath, RTL_NUMBER_OF(LogCtx->FilePath) - 1, L"%ws", FilePath);
    InitializeCriticalSection(&LogCtx->Lock);

    LogCtx->hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!LogCtx->hEvent) {
        return GetLastError();
    }

    LogCtx->hFile = CreateFile(LogCtx->FilePath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (LogCtx->hFile == INVALID_HANDLE_VALUE) {
        CloseHandle(LogCtx->hEvent);
        DeleteCriticalSection(&LogCtx->Lock);
        return GetLastError();
    }

    LogCtx->hThread = CreateThread(NULL, 0, LogThreadRoutine, LogCtx, 0, NULL);
    if (!LogCtx->hThread) {
        CloseHandle(LogCtx->hEvent);
        CloseHandle(LogCtx->hFile);
        DeleteCriticalSection(&LogCtx->Lock);
        return GetLastError();
    }

    return 0;
}

VOID LogRelease(PLOG_CONTEXT LogCtx)
{
    PLIST_ENTRY ListEntry;
    PLOG_ENTRY LogEntry;

    LogCtx->Stopping = 1;

    WaitForSingleObject(LogCtx->hThread, INFINITE);

    EnterCriticalSection(&LogCtx->Lock);
    while (!IsListEmpty(&LogCtx->ListHead)) {
        ListEntry = RemoveHeadList(&LogCtx->ListHead);
        LogEntry = CONTAINING_RECORD(ListEntry, LOG_ENTRY, ListEntry);
        LogWriteEntry(LogCtx, LogEntry);
        free(LogEntry);
    }
    FlushFileBuffers(LogCtx->hFile);
    LeaveCriticalSection(&LogCtx->Lock);
    DeleteCriticalSection(&LogCtx->Lock);
    CloseHandle(LogCtx->hEvent);
    CloseHandle(LogCtx->hFile);
    CloseHandle(LogCtx->hThread);
}

BOOLEAN LogEntryEnqueue(PLOG_CONTEXT LogCtx, PLOG_ENTRY LogEntry)
{
    BOOLEAN Inserted;
    if (LogCtx->Stopping)
        return FALSE;

    EnterCriticalSection(&LogCtx->Lock);
    if (!LogCtx->Stopping) {
        InsertTailList(&LogCtx->ListHead, &LogEntry->ListEntry);
        Inserted = TRUE;
    } else
        Inserted = FALSE;
    LeaveCriticalSection(&LogCtx->Lock);
    if (Inserted)
        SetEvent(LogCtx->hEvent);

    return Inserted;
}

VOID Log(PLOG_CONTEXT LogCtx, ULONG Level, PCHAR File, ULONG Line, PCHAR Func, PCHAR Fmt, va_list Args)
{
    PLOG_ENTRY LogEntry;
    PCHAR BufPos;
    ULONG BufSize, BufLeft;
    SYSTEMTIME Time;

    LogEntry = malloc(sizeof(*LogEntry));
    if (!LogEntry)
        return;

    LogEntry->BufUsed = 0;
    BufPos = LogEntry->Buf;
    BufSize = sizeof(LogEntry->Buf);
    BufLeft = BufSize;

    switch (Level) {
    case LOG_INF:
        WriteMsg(&BufPos, &BufLeft, "INF");
        break;
    case LOG_ERR:
        WriteMsg(&BufPos, &BufLeft, "ERR");
        break;
    case LOG_DBG:
        WriteMsg(&BufPos, &BufLeft, "DBG");
        break;
    case LOG_WRN:
        WriteMsg(&BufPos, &BufLeft, "WRN");
        break;
    default:
        WriteMsg(&BufPos, &BufLeft, "UNK");
        break;
    }

    GetSystemTime(&Time);
    WriteMsg(&BufPos, &BufLeft," %02d:%02d:%02d.%03d ",
             Time.wHour, Time.wMinute,
             Time.wSecond, Time.wMilliseconds);

    WriteMsg(&BufPos, &BufLeft,"t%x", GetCurrentThreadId());
    WriteMsg(&BufPos, &BufLeft," %s():%s:%d: ", Func, TruncatePath(File), Line);

    WriteMsg2(&BufPos, &BufLeft, Fmt, Args);
    if (WriteMsg(&BufPos, &BufLeft, "\r\n") <= 0) {
        LogEntry->Buf[BufSize-2] = '\r';
        LogEntry->Buf[BufSize-1] = '\n';
        LogEntry->BufUsed = BufSize;
    } else
        LogEntry->BufUsed = BufSize - BufLeft;

    if (!LogEntryEnqueue(LogCtx, LogEntry))
        free(LogEntry);
}

DWORD GlobalLogInit(PWCHAR FilePath)
{
    return LogInit(&g_LogCtx, FilePath);
}

VOID GlobalLogRelease()
{
    LogRelease(&g_LogCtx);
}

VOID GlobalLog(ULONG Level, PCHAR File, ULONG Line, PCHAR Func, PCHAR Fmt, ...)
{
    va_list Args;

    va_start(Args, Fmt);
    Log(&g_LogCtx, Level, File, Line, Func, Fmt, Args);
    va_end(Args);
}