



#include "Tasks/WatsonxVRBaseTask.h"
#include "Management/WatsonxVRSettings.h"
#include "WatsonxVRInternalFuncs.h"
#include "LogWatsonxVR.h"

#include <HttpModule.h>
#include <Interfaces/IHttpRequest.h>
#include <Interfaces/IHttpResponse.h>
#include <Dom/JsonObject.h>
#include <Serialization/JsonWriter.h>
#include <Serialization/JsonReader.h>
#include <Serialization/JsonSerializer.h>
#include <Misc/ScopeTryLock.h>
#include <Async/Async.h>

#if WITH_EDITOR
#include <Editor.h>
#endif

#ifdef UE_INLINE_GENERATED_CPP_BY_NAME
#include UE_INLINE_GENERATED_CPP_BY_NAME(WatsonxVRBaseTask)
#endif

void UWatsonxVRBaseTask::Activate()
{
	Super::Activate();

	UE_LOG(LogWatsonxVR, Display, TEXT("%s (%d): Activating task"), *FString(__func__), GetUniqueID());

	bIsTaskActive = true;
	if (!CommonOptions.Endpoint.EndsWith(TEXT("/")))
	{
		CommonOptions.Endpoint += TEXT("/");
	}

	if (!CanActivateTask())
	{
		UE_LOG(LogWatsonxVR, Error, TEXT("%s (%d): Failed to activate task."), *FString(__func__), GetUniqueID());
		RequestFailed.Broadcast();
		SetReadyToDestroy();
		return;
	}

	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this]
	{
		SendRequest();
	});

#if WITH_EDITOR
	if (bIsEditorTask)
	{
		SetFlags(RF_Standalone);
	}
	else
	{
		FEditorDelegates::PrePIEEnded.AddUObject(this, &UWatsonxVRBaseTask::PrePIEEnded);
	}
#endif
}

void UWatsonxVRBaseTask::StopWatsonxVRTask()
{
	FScopeLock Lock(&Mutex);

	if (!bIsTaskActive)
	{
		return;
	}

	UE_LOG(LogWatsonxVR, Display, TEXT("%s (%d): Stopping task"), *FString(__func__), GetUniqueID());

	bIsTaskActive = false;

	if (HttpRequest.IsValid())
	{
		HttpRequest->CancelRequest();
		HttpRequest.Reset();
	}

	SetReadyToDestroy();
}

void UWatsonxVRBaseTask::SetReadyToDestroy()
{
	FScopeLock Lock(&Mutex);

	if (bIsReadyToDestroy)
	{
		return;
	}

	UE_LOG(LogWatsonxVR, Display, TEXT("%s (%d): Setting task as Ready to Destroy"), *FString(__func__), GetUniqueID());

#if WITH_EDITOR
	if (bIsEditorTask)
	{
		ClearFlags(RF_Standalone);

#if ENGINE_MAJOR_VERSION >= 5
		MarkAsGarbage();
#else
        MarkPendingKill();
#endif
	}

	if (FEditorDelegates::PrePIEEnded.IsBoundToObject(this))
	{
		FEditorDelegates::PrePIEEnded.RemoveAll(this);
	}
#endif

	bIsReadyToDestroy = true;
	bIsTaskActive = false;

	Super::SetReadyToDestroy();
}

const FWatsonxVRCommonOptions UWatsonxVRBaseTask::GetCommonOptions() const
{
	return CommonOptions;
}

#if WITH_EDITOR
void UWatsonxVRBaseTask::PrePIEEnded(bool bIsSimulating)
{
	if (!IsValid(this))
	{
		return;
	}

	UE_LOG(LogWatsonxVR, Display, TEXT("%s (%d): Trying to finish task due to PIE end"), *FString(__func__), GetUniqueID());

	bEndingPIE = true;
	StopWatsonxVRTask();
}
#endif

bool UWatsonxVRBaseTask::CanActivateTask() const
{
	if (WatsonxVR::Internal::HasEmptyParam(GetCommonOptions().APIKey))
	{
		UE_LOG(LogWatsonxVR, Error, TEXT("%s (%d): Can't activate task: Invalid API Key."), *FString(__func__), GetUniqueID());
		return false;
	}

	return true;
}

bool UWatsonxVRBaseTask::CanBindProgress() const
{
	return true;
}

FString UWatsonxVRBaseTask::GetEndpointURL() const
{
	return FString();
}

void UWatsonxVRBaseTask::InitializeRequest()
{
	FScopeLock Lock(&Mutex);

	UE_LOG(LogWatsonxVR_Internal, Display, TEXT("%s (%d): Initializing request object"), *FString(__func__), GetUniqueID());

	HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(GetEndpointURL());
	HttpRequest->SetVerb("POST");
	HttpRequest->SetHeader("Content-Type", "application/json");
	HttpRequest->SetHeader("Authorization", FString::Format(TEXT("Bearer {0}"), {GetCommonOptions().APIKey.ToString()}));
}

void UWatsonxVRBaseTask::BindRequestCallbacks()
{
	FScopeLock Lock(&Mutex);

	if (!HttpRequest.IsValid())
	{
		return;
	}

	UE_LOG(LogWatsonxVR_Internal, Display, TEXT("%s (%d): Binding callbacks"), *FString(__func__), GetUniqueID());

	if (CanBindProgress())
	{
		HttpRequest->OnRequestProgress().BindLambda([this](const FHttpRequestPtr& Request, int32 BytesSent, int32 BytesReceived)
		{
			const FScopeTryLock Lock(&Mutex);

			if (!Lock.IsLocked() || !IsValid(this) || !bIsTaskActive || !Request.IsValid())
			{
				return;
			}

			if (const FHttpResponsePtr Response = Request->GetResponse(); Response.IsValid())
			{
				OnProgressUpdated(Response->GetContentAsString(), BytesSent, BytesReceived);
			}
		});
	}

	HttpRequest->OnProcessRequestComplete().BindLambda([this](FHttpRequestPtr Request, const FHttpResponsePtr& RequestResponse, bool bWasSuccessful)
	{
		const FScopeTryLock Lock(&Mutex);

		if (!Lock.IsLocked() || !IsValid(this) || !bIsTaskActive)
		{
			return;
		}

		OnProgressCompleted(RequestResponse->GetContentAsString(), bWasSuccessful);
		SetReadyToDestroy();
	});
}

void UWatsonxVRBaseTask::SendRequest()
{
	FScopeLock Lock(&Mutex);

	InitializeRequest();
	const FString ContentString = SetRequestContent();
	BindRequestCallbacks();

	if (!HttpRequest.IsValid())
	{
		UE_LOG(LogWatsonxVR, Error, TEXT("%s (%d): Failed to send request: Request object is invalid"), *FString(__func__), GetUniqueID());

		AsyncTask(ENamedThreads::GameThread, [this]
		{
			RequestFailed.Broadcast();
			SetReadyToDestroy();
		});

		return;
	}

	UE_LOG(LogWatsonxVR, Display, TEXT("%s (%d): Sending request"), *FString(__func__), GetUniqueID());

	if (HttpRequest->ProcessRequest())
	{
		UE_LOG(LogWatsonxVR, Display, TEXT("%s (%d): Request sent"), *FString(__func__), GetUniqueID());
		UE_LOG(LogWatsonxVR_Internal, Display, TEXT("%s (%d): Request content body:\n%s"), *FString(__func__), GetUniqueID(), *ContentString);

		AsyncTask(ENamedThreads::GameThread, [this]
		{
			RequestSent.Broadcast();
		});
	}
	else
	{
		UE_LOG(LogWatsonxVR, Error, TEXT("%s (%d): Failed to initialize the request process"), *FString(__func__), GetUniqueID());
		AsyncTask(ENamedThreads::GameThread, [this]
		{
			RequestFailed.Broadcast();
			SetReadyToDestroy();
		});
	}
}

const bool UWatsonxVRBaseTask::CheckError(const TSharedPtr<FJsonObject>& JsonObject, FWatsonxVRCommonError& OutputError) const
{
	if (JsonObject->HasField("error"))
	{
		const TSharedPtr<FJsonObject> ErrorObj = JsonObject->GetObjectField("error");
		if (FString ErrorMessage; ErrorObj->TryGetStringField("message", ErrorMessage))
		{
			OutputError.Message = *ErrorMessage;
		}
		if (FString ErrorCode; ErrorObj->TryGetStringField("code", ErrorCode))
		{
			OutputError.Code = *ErrorCode;
		}
		if (FString ErrorType; ErrorObj->TryGetStringField("type", ErrorType))
		{
			OutputError.Type = *ErrorType;
		}

		return true;
	}

	return false;
}

bool UWatsonxVRTaskStatus::IsTaskActive(const UWatsonxVRBaseTask* Test)
{
	return IsValid(Test) && Test->bIsTaskActive;
}

bool UWatsonxVRTaskStatus::IsTaskReadyToDestroy(const UWatsonxVRBaseTask* Test)
{
	return IsValid(Test) && Test->bIsReadyToDestroy;
}

bool UWatsonxVRTaskStatus::IsTaskStillValid(const UWatsonxVRBaseTask* Test)
{
	bool bOutput = IsValid(Test) && !IsTaskReadyToDestroy(Test);

#if WITH_EDITOR
	bOutput = bOutput && !Test->bEndingPIE;
#endif

	return bOutput;
}