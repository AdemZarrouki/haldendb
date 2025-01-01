#pragma once

enum class ErrorCode 
{
    Success,
    Error,
    InsertFailed,
    ChildSplitCalledOnLeafNode,
    KeyDoesNotExist,
    TreeEmpty
};
