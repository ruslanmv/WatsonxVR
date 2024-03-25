



#include "Tasks/AppChatRequest.h"
#include <Utils/AppHelper.h>
#include <Management/AppSettings.h>
#include <AppInternalFuncs.h>
#include <LogApp.h>

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
#include UE_INLINE_GENERATED_CPP_BY_NAME(AppChatRequest)
#endif

#if WITH_EDITOR
UAppChatRequest* UAppChatRequest::EditorTask(const TArray<FAppChatMessage>& Messages, const FAppChatOptions& Options)
{
	UAppChatRequest* const NewAsyncTask = SendMessages_CustomOptions(GEditor->GetEditorWorldContext().World(), Messages,
	                                                                     TArray<FAppFunction>(), FAppCommonOptions(), Options);
	NewAsyncTask->bIsEditorTask = true;

	return NewAsyncTask;
}
#endif

UAppChatRequest* UAppChatRequest::SendMessage_DefaultOptions(UObject* const WorldContextObject, const FString& Message,
                                                                     const TArray<FAppFunction>& Functions)
{
	return SendMessage_CustomOptions(WorldContextObject, Message, Functions, FAppCommonOptions(), FAppChatOptions());
}

UAppChatRequest* UAppChatRequest::SendMessages_DefaultOptions(UObject* const WorldContextObject, const TArray<FAppChatMessage>& Messages,
                                                                      const TArray<FAppFunction>& Functions)
{
	return SendMessages_CustomOptions(WorldContextObject, Messages, Functions, FAppCommonOptions(), FAppChatOptions());
}

UAppChatRequest* UAppChatRequest::SendMessage_CustomOptions(UObject* const WorldContextObject, const FString& Message,
                                                                    const TArray<FAppFunction>& Functions,
                                                                    const FAppCommonOptions CommonOptions, const FAppChatOptions ChatOptions)
{
	return SendMessages_CustomOptions(WorldContextObject, {FAppChatMessage(EAppChatRole::User, Message)}, Functions, CommonOptions,
	                                  ChatOptions);
}

UAppChatRequest* UAppChatRequest::SendMessages_CustomOptions(UObject* const WorldContextObject, const TArray<FAppChatMessage>& Messages,
                                                                     const TArray<FAppFunction>& Functions,
                                                                     const FAppCommonOptions CommonOptions, const FAppChatOptions ChatOptions)
{
	UAppChatRequest* const NewAsyncTask = NewObject<UAppChatRequest>();
	NewAsyncTask->Messages = Messages;
	NewAsyncTask->CommonOptions = CommonOptions;
	NewAsyncTask->ChatOptions = ChatOptions;
	NewAsyncTask->Functions = Functions;

	NewAsyncTask->RegisterWithGameInstance(WorldContextObject);

	return NewAsyncTask;
}

bool UAppChatRequest::CanActivateTask() const
{
	if (!Super::CanActivateTask())
	{
		return false;
	}

	if (App::Internal::HasEmptyParam(Messages))
	{
		UE_LOG(LogApp, Error, TEXT("%s (%d): Can't activate task: Invalid Messages."), *FString(__func__), GetUniqueID());
		return false;
	}

	return true;
}

bool UAppChatRequest::CanBindProgress() const
{
	return GetChatOptions().bStream;
}

FString UAppChatRequest::GetEndpointURL() const
{
	return FString::Format(TEXT("{0}/{1}"), {
		                       GetCommonOptions().Endpoint,
		                       UAppHelper::GetEndpointForModel(GetChatOptions().Model, GetCommonOptions().bIsAzureOpenAI,
		                                                           GetCommonOptions().AzureOpenAIAPIVersion)
	                       });
}

const FAppChatOptions UAppChatRequest::GetChatOptions() const
{
	return ChatOptions;
}

FString UAppChatRequest::SetRequestContent()
{
	FScopeLock Lock(&Mutex);

	if (!HttpRequest.IsValid())
	{
		return FString();
	}

	UE_LOG(LogApp_Internal, Display, TEXT("%s (%d): Mounting content"), *FString(__func__), GetUniqueID());

	const TSharedPtr<FJsonObject> JsonRequest = MakeShared<FJsonObject>();
	JsonRequest->SetStringField("model", UAppHelper::ModelToName(GetChatOptions().Model).ToString().ToLower());
	JsonRequest->SetNumberField("max_tokens", GetChatOptions().MaxTokens);
	JsonRequest->SetNumberField("temperature", GetChatOptions().Temperature);
	JsonRequest->SetNumberField("top_p", GetChatOptions().TopP);
	JsonRequest->SetNumberField("n", GetChatOptions().Choices);
	JsonRequest->SetNumberField("presence_penalty", GetChatOptions().PresencePenalty);
	JsonRequest->SetNumberField("frequency_penalty", GetChatOptions().FrequencyPenalty);
	JsonRequest->SetBoolField("stream", GetChatOptions().bStream);

	if (!App::Internal::HasEmptyParam(GetCommonOptions().User))
	{
		JsonRequest->SetStringField("user", GetCommonOptions().User.ToString());
	}

	if (!App::Internal::HasEmptyParam(GetChatOptions().Stop))
	{
		TArray<TSharedPtr<FJsonValue>> StopJson;
		for (const FName& Iterator : GetChatOptions().Stop)
		{
			StopJson.Add(MakeShared<FJsonValueString>(Iterator.ToString()));
		}

		JsonRequest->SetArrayField("stop", StopJson);
	}

	if (!App::Internal::HasEmptyParam(GetChatOptions().LogitBias))
	{
		const TSharedPtr<FJsonObject> LogitBiasJson = MakeShared<FJsonObject>();
		for (const TPair<int32, float>& Iterator : GetChatOptions().LogitBias)
		{
			LogitBiasJson->SetNumberField(FString::FromInt(Iterator.Key), Iterator.Value);
		}

		JsonRequest->SetObjectField("logit_bias", LogitBiasJson);
	}

	if (UAppHelper::ModelSupportsChat(GetChatOptions().Model))
	{
		UE_LOG(LogApp_Internal, Display, TEXT("%s (%d): Selected model supports Chat API. Mounting section history."), *FString(__func__),
		       GetUniqueID());

		TArray<TSharedPtr<FJsonValue>> MessagesJson;
		for (const FAppChatMessage& Iterator : Messages)
		{
			MessagesJson.Add(Iterator.GetMessage());
		}

		JsonRequest->SetArrayField("messages", MessagesJson);

		if (!Functions.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> FunctionsJson;
			for (const FAppFunction& Iterator : Functions)
			{
				FunctionsJson.Add(Iterator.GetFunction());
			}

			JsonRequest->SetArrayField("functions", FunctionsJson);
		}
	}
	else
	{
		UE_LOG(LogApp_Internal, Display, TEXT("%s (%d): Selected model does not supports Chat API. Using last message as prompt content."),
		       *FString(__func__), GetUniqueID());
		JsonRequest->SetStringField("prompt", Messages.Top().Content);
	}

	FString RequestContentString;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&RequestContentString);
	FJsonSerializer::Serialize(JsonRequest.ToSharedRef(), Writer);

	HttpRequest->SetContentAsString(RequestContentString);

	return RequestContentString;
}

void UAppChatRequest::OnProgressUpdated(const FString& Content, int32 BytesSent, int32 BytesReceived)
{
	FScopeLock Lock(&Mutex);

	if (App::Internal::HasEmptyParam(Content))
	{
		return;
	}

	TArray<FString> Deltas = GetDeltasFromContent(Content);

	UE_LOG(LogApp_Internal, Display, TEXT("%s (%d): Progress Updated"), *FString(__func__), GetUniqueID());
	UE_LOG(LogApp_Internal, Display, TEXT("%s (%d): Content: %s; Bytes Sent: %d; Bytes Received: %d"), *FString(__func__), GetUniqueID(),
	       *Deltas.Top(), BytesSent, BytesReceived);

	DeserializeStreamedResponse(Deltas);

	if (!Response.bSuccess)
	{
		return;
	}

	if (!bInitialized)
	{
		bInitialized = true;

		AsyncTask(ENamedThreads::GameThread, [this]
		{
			FScopeLock Lock(&Mutex);
			ProgressStarted.Broadcast(Response);
		});
	}

	AsyncTask(ENamedThreads::GameThread, [this]
	{
		const FScopeTryLock Lock(&Mutex);

		if (Lock.IsLocked())
		{
			ProgressUpdated.Broadcast(Response);
		}
	});
}

void UAppChatRequest::OnProgressCompleted(const FString& Content, const bool bWasSuccessful)
{
	FScopeLock Lock(&Mutex);

	if (!bWasSuccessful || App::Internal::HasEmptyParam(Content))
	{
		UE_LOG(LogApp, Error, TEXT("%s (%d): Request failed"), *FString(__func__), GetUniqueID());
		AsyncTask(ENamedThreads::GameThread, [this]
		{
			RequestFailed.Broadcast();
		});

		return;
	}

	UE_LOG(LogApp_Internal, Display, TEXT("%s (%d): Process Completed"), *FString(__func__), GetUniqueID());
	UE_LOG(LogApp_Internal, Display, TEXT("%s (%d): Content: %s"), *FString(__func__), GetUniqueID(), *Content);

	if (!GetChatOptions().bStream)
	{
		DeserializeSingleResponse(Content);
	}
	else
	{
		const TArray<FString> Deltas = GetDeltasFromContent(Content);
		DeserializeStreamedResponse(Deltas);
	}

	if (Response.bSuccess)
	{
		AsyncTask(ENamedThreads::GameThread, [this]
		{
			FScopeLock Lock(&Mutex);

			if (!GetChatOptions().bStream)
			{
				ProgressStarted.Broadcast(Response);
			}

			ProcessCompleted.Broadcast(Response);
		});
	}
	else
	{
		UE_LOG(LogApp, Error, TEXT("%s (%d): Request failed"), *FString(__func__), GetUniqueID());
		AsyncTask(ENamedThreads::GameThread, [this]
		{
			FScopeLock Lock(&Mutex);
			ErrorReceived.Broadcast(Response);
		});
	}
}

TArray<FString> UAppChatRequest::GetDeltasFromContent(const FString& Content) const
{
	TArray<FString> Deltas;
	Content.ParseIntoArray(Deltas, TEXT("data: "));

	if (Deltas.Top().Contains("[done]", ESearchCase::IgnoreCase))
	{
		Deltas.Pop();
	}

	if (App::Internal::HasEmptyParam(Deltas))
	{
		Deltas.Add(Content);
	}

	return Deltas;
}

void UAppChatRequest::DeserializeStreamedResponse(const TArray<FString>& Deltas)
{
	FScopeLock Lock(&Mutex);

	Response.Choices.Empty(Deltas.Num());
	for (const FString& Delta : Deltas)
	{
		DeserializeSingleResponse(Delta);
	}
}

void UAppChatRequest::DeserializeSingleResponse(const FString& Content)
{
	FScopeLock Lock(&Mutex);

	if (App::Internal::HasEmptyParam(Content))
	{
		return;
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
	TSharedPtr<FJsonObject> JsonResponse = MakeShared<FJsonObject>();
	FJsonSerializer::Deserialize(Reader, JsonResponse);

	if (CheckError(JsonResponse, Response.Error))
	{
		Response.bSuccess = false;
		return;
	}

	Response.bSuccess = true;

	Response.ID = *JsonResponse->GetStringField("id");
	Response.Object = *JsonResponse->GetStringField("object");
	Response.Created = JsonResponse->GetNumberField("created");

	const TArray<TSharedPtr<FJsonValue>> ChoicesArr = JsonResponse->GetArrayField("choices");

	for (auto Iterator = ChoicesArr.CreateConstIterator(); Iterator; ++Iterator)
	{
		const TSharedPtr<FJsonObject> ChoiceObj = (*Iterator)->AsObject();
		const int32 ChoiceIndex = ChoiceObj->GetIntegerField("index");

		FAppChatChoice* Choice = Response.Choices.FindByPredicate([this, ChoiceIndex](const FAppChatChoice& Element)
		{
			return Element.Index == ChoiceIndex;
		});

		if (!Choice)
		{
			FAppChatChoice NewChoice;
			NewChoice.Index = ChoiceIndex;
			Choice = &Response.Choices.Add_GetRef(NewChoice);
		}

		if (const TSharedPtr<FJsonObject>* MessageObj; ChoiceObj->TryGetObjectField("message", MessageObj))
		{
			if (FString RoleStr; (*MessageObj)->TryGetStringField("role", RoleStr))
			{
				Choice->Message.Role = RoleStr == "user" ? EAppChatRole::User : EAppChatRole::Assistant;
			}

			if (FString ContentStr; (*MessageObj)->TryGetStringField("content", ContentStr))
			{
				Choice->Message.Content = ContentStr;
			}

			if (const TSharedPtr<FJsonObject>* FunctionObj; (*MessageObj)->TryGetObjectField("function_call", FunctionObj))
			{
				if (FString FunctionNameStr; (*FunctionObj)->TryGetStringField("name", FunctionNameStr))
				{
					Choice->Message.FunctionCall.Name = *FunctionNameStr;
				}
				if (FString FunctionArgumentsStr; (*FunctionObj)->TryGetStringField("arguments", FunctionArgumentsStr))
				{
					Choice->Message.FunctionCall.Arguments = FunctionArgumentsStr;
				}
			}
		}
		else if (const TSharedPtr<FJsonObject>* DeltaObj; ChoiceObj->TryGetObjectField("delta", DeltaObj))
		{
			if (FString RoleStr; (*DeltaObj)->TryGetStringField("role", RoleStr))
			{
				Choice->Message.Role = UAppHelper::NameToRole(*RoleStr);
			}
			else if (FString ContentStr; (*DeltaObj)->TryGetStringField("content", ContentStr))
			{
				Choice->Message.Content += ContentStr;
			}

			if (const TSharedPtr<FJsonObject>* FunctionObj; (*DeltaObj)->TryGetObjectField("function_call", FunctionObj))
			{
				if (FString FunctionNameStr; (*FunctionObj)->TryGetStringField("name", FunctionNameStr))
				{
					Choice->Message.FunctionCall.Name = *FunctionNameStr;
				}
				if (FString FunctionArgumentsStr; (*FunctionObj)->TryGetStringField("arguments", FunctionArgumentsStr))
				{
					Choice->Message.FunctionCall.Arguments += FunctionArgumentsStr;
				}
			}
		}
		else if (FString MessageText; ChoiceObj->TryGetStringField("text", MessageText))
		{
			Choice->Message.Role = EAppChatRole::Assistant;
			Choice->Message.Content += MessageText;
		}

		while (Choice->Message.Content.StartsWith("\n"))
		{
			Choice->Message.Content.RemoveAt(0);
		}

		if (FString FinishReasonStr; ChoiceObj->TryGetStringField("finish_reason", FinishReasonStr))
		{
			Choice->FinishReason = *FinishReasonStr;
		}
	}

	if (const TSharedPtr<FJsonObject>* UsageObj; JsonResponse->TryGetObjectField("usage", UsageObj))
	{
		Response.Usage = FAppChatUsage((*UsageObj)->GetNumberField("prompt_tokens"), (*UsageObj)->GetNumberField("completion_tokens"),
		                                   (*UsageObj)->GetNumberField("total_tokens"));
	}
}

UAppChatRequest* UAppChatHelper::CastToAppChatRequest(UObject* const Object)
{
	return Cast<UAppChatRequest>(Object);
}