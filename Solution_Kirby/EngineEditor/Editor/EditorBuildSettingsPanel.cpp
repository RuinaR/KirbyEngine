#include "pch.h"
#include "EditorBuildSettingsPanel.h"
#include "BuildSettingsManager.h"
#include "EditorSceneWorkflow.h"
#include "ObjectManager.h"
#include "SceneDataManager.h"
#include <chrono>
#include <fstream>
#include <future>
#include <mutex>
#include <regex>
#include <set>

namespace
{
	BuildSettingsData g_buildSettingsUiState;
	bool g_buildSettingsUiLoaded = false;
	std::string g_buildGameStatusMessage;
	std::string g_buildGameStageMessage;
	int g_buildGameCurrentStage = 0;
	int g_buildGameTotalStages = 0;
	bool g_buildGameRunning = false;
	std::future<void> g_buildGameFuture;
	std::mutex g_buildGameStateMutex;

	struct BuildGameUiSnapshot
	{
		bool running = false;
		int currentStage = 0;
		int totalStages = 0;
		std::string stageMessage;
		std::string resultMessage;
	};

	struct BuildSettingsValidationState
	{
		bool hasBlockingError = false;
		bool hasMissingSceneFile = false;
		bool hasEmptyScenes = false;
		bool hasEmptyStartScene = false;
		bool hasStartSceneOutsideScenes = false;
		std::vector<std::string> missingSceneNames;
	};

	BuildGameUiSnapshot GetBuildGameUiSnapshot()
	{
		std::lock_guard<std::mutex> lock(g_buildGameStateMutex);

		BuildGameUiSnapshot snapshot;
		snapshot.running = g_buildGameRunning;
		snapshot.currentStage = g_buildGameCurrentStage;
		snapshot.totalStages = g_buildGameTotalStages;
		snapshot.stageMessage = g_buildGameStageMessage;
		snapshot.resultMessage = g_buildGameStatusMessage;
		return snapshot;
	}

	void SetBuildGameStage(int currentStage, int totalStages, const std::string& stageMessage)
	{
		std::lock_guard<std::mutex> lock(g_buildGameStateMutex);
		g_buildGameRunning = true;
		g_buildGameCurrentStage = currentStage;
		g_buildGameTotalStages = totalStages;
		g_buildGameStageMessage = stageMessage;
	}

	void SetBuildGameResult(const std::string& resultMessage)
	{
		std::lock_guard<std::mutex> lock(g_buildGameStateMutex);
		g_buildGameRunning = false;
		g_buildGameStageMessage.clear();
		g_buildGameStatusMessage = resultMessage;
	}

	void BeginBuildGameAsync(int totalStages, const std::string& initialStageMessage)
	{
		std::lock_guard<std::mutex> lock(g_buildGameStateMutex);
		g_buildGameRunning = true;
		g_buildGameCurrentStage = 0;
		g_buildGameTotalStages = totalStages;
		g_buildGameStageMessage = initialStageMessage;
		g_buildGameStatusMessage.clear();
	}

	bool IsBuildGameRunning()
	{
		std::lock_guard<std::mutex> lock(g_buildGameStateMutex);
		return g_buildGameRunning;
	}

	void PollBuildGameAsync()
	{
		if (!g_buildGameFuture.valid())
		{
			return;
		}

		if (g_buildGameFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
		{
			return;
		}

		try
		{
			g_buildGameFuture.get();
		}
		catch (const std::exception& ex)
		{
			SetBuildGameResult(std::string("비동기 빌드 작업에서 예외가 발생했습니다: ") + ex.what());
		}
		catch (...)
		{
			SetBuildGameResult("비동기 빌드 작업에서 알 수 없는 예외가 발생했습니다.");
		}
	}

	std::string GetCurrentSceneName()
	{
		if (WindowFrame::GetInstance() == nullptr)
		{
			return std::string();
		}

		const char* sceneName = WindowFrame::GetInstance()->GetCurrentSceneName();
		return sceneName != nullptr ? sceneName : "";
	}

	void EnsureBuildSettingsUiLoaded()
	{
		if (g_buildSettingsUiLoaded)
		{
			return;
		}

		const std::vector<std::string> sceneNames = SceneDataManager::GetSceneFileList();
		BuildSettingsManager::LoadOrCreateDefault(g_buildSettingsUiState, sceneNames);
		g_buildSettingsUiLoaded = true;
	}

	bool IsSceneIncludedInBuildSettings(const std::string& sceneName)
	{
		for (std::vector<std::string>::const_iterator itr = g_buildSettingsUiState.scenes.begin(); itr != g_buildSettingsUiState.scenes.end(); ++itr)
		{
			if (*itr == sceneName)
			{
				return true;
			}
		}

		return false;
	}

	void IncludeSceneInBuildSettings(const std::string& sceneName)
	{
		if (sceneName.empty() || IsSceneIncludedInBuildSettings(sceneName))
		{
			return;
		}

		g_buildSettingsUiState.scenes.push_back(sceneName);
		if (g_buildSettingsUiState.startScene.empty())
		{
			g_buildSettingsUiState.startScene = sceneName;
		}

		BuildSettingsManager::Validate(g_buildSettingsUiState);
	}

	void ExcludeSceneFromBuildSettings(const std::string& sceneName)
	{
		std::vector<std::string>& scenes = g_buildSettingsUiState.scenes;
		scenes.erase(std::remove(scenes.begin(), scenes.end(), sceneName), scenes.end());
		BuildSettingsManager::Validate(g_buildSettingsUiState);
	}

	bool CanUseCurrentSceneForBuildSettings(std::string* outSceneName = nullptr)
	{
		const std::string currentSceneName = GetCurrentSceneName();
		if (outSceneName != nullptr)
		{
			*outSceneName = currentSceneName;
		}

		if (currentSceneName.empty())
		{
			return false;
		}

		return SceneDataManager::Exists(currentSceneName);
	}

	BuildSettingsValidationState ValidateBuildSettingsUiState(const BuildSettingsData& settings)
	{
		BuildSettingsValidationState validation;
		validation.hasEmptyScenes = settings.scenes.empty();
		validation.hasEmptyStartScene = settings.startScene.empty();
		validation.hasStartSceneOutsideScenes = !settings.startScene.empty() && !IsSceneIncludedInBuildSettings(settings.startScene);

		for (std::vector<std::string>::const_iterator itr = settings.scenes.begin(); itr != settings.scenes.end(); ++itr)
		{
			if (SceneDataManager::Exists(*itr))
			{
				continue;
			}

			validation.hasMissingSceneFile = true;
			validation.missingSceneNames.push_back(*itr);
		}

		validation.hasBlockingError =
			validation.hasEmptyScenes ||
			validation.hasEmptyStartScene ||
			validation.hasStartSceneOutsideScenes ||
			validation.hasMissingSceneFile;

		return validation;
	}

	bool FileExists(const std::string& path)
	{
		const DWORD attributes = GetFileAttributesA(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	bool DirectoryExists(const std::string& path)
	{
		const DWORD attributes = GetFileAttributesA(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}

	bool EnsureDirectoryExists(const std::string& path)
	{
		if (path.empty())
		{
			return false;
		}

		if (DirectoryExists(path))
		{
			return true;
		}

		const size_t separatorPos = path.find_last_of("\\/");
		if (separatorPos != std::string::npos)
		{
			const std::string parentPath = path.substr(0, separatorPos);
			if (!parentPath.empty() && !DirectoryExists(parentPath) && !EnsureDirectoryExists(parentPath))
			{
				return false;
			}
		}

		return CreateDirectoryA(path.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS;
	}

	bool CopySingleFile(const std::string& sourcePath, const std::string& destinationPath)
	{
		if (!FileExists(sourcePath))
		{
			return false;
		}

		const size_t fileNamePos = destinationPath.find_last_of("\\/");
		if (fileNamePos != std::string::npos)
		{
			if (!EnsureDirectoryExists(destinationPath.substr(0, fileNamePos)))
			{
				return false;
			}
		}

		return CopyFileA(sourcePath.c_str(), destinationPath.c_str(), FALSE) != FALSE;
	}

	bool CopyDirectoryRecursive(const std::string& sourceDir, const std::string& destinationDir)
	{
		if (!DirectoryExists(sourceDir))
		{
			return false;
		}

		if (!EnsureDirectoryExists(destinationDir))
		{
			return false;
		}

		WIN32_FIND_DATAA findData = {};
		HANDLE findHandle = FindFirstFileA((sourceDir + "\\*").c_str(), &findData);
		if (findHandle == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		bool succeeded = true;
		do
		{
			const char* fileName = findData.cFileName;
			if (strcmp(fileName, ".") == 0 || strcmp(fileName, "..") == 0)
			{
				continue;
			}

			const std::string sourcePath = sourceDir + "\\" + fileName;
			const std::string targetPath = destinationDir + "\\" + fileName;
			if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			{
				if (!CopyDirectoryRecursive(sourcePath, targetPath))
				{
					succeeded = false;
					break;
				}
				continue;
			}

			if (!CopySingleFile(sourcePath, targetPath))
			{
				succeeded = false;
				break;
			}
		}
		while (FindNextFileA(findHandle, &findData) != FALSE);

		FindClose(findHandle);
		return succeeded;
	}

	bool DeleteDirectoryRecursive(const std::string& directoryPath)
	{
		if (!DirectoryExists(directoryPath))
		{
			return true;
		}

		WIN32_FIND_DATAA findData = {};
		HANDLE findHandle = FindFirstFileA((directoryPath + "\\*").c_str(), &findData);
		if (findHandle == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		bool succeeded = true;
		do
		{
			const char* fileName = findData.cFileName;
			if (strcmp(fileName, ".") == 0 || strcmp(fileName, "..") == 0)
			{
				continue;
			}

			const std::string childPath = directoryPath + "\\" + fileName;
			if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			{
				if (!DeleteDirectoryRecursive(childPath))
				{
					succeeded = false;
					break;
				}
				continue;
			}

			SetFileAttributesA(childPath.c_str(), FILE_ATTRIBUTE_NORMAL);
			if (DeleteFileA(childPath.c_str()) == FALSE)
			{
				succeeded = false;
				break;
			}
		}
		while (FindNextFileA(findHandle, &findData) != FALSE);

		FindClose(findHandle);
		if (!succeeded)
		{
			return false;
		}

		SetFileAttributesA(directoryPath.c_str(), FILE_ATTRIBUTE_NORMAL);
		return RemoveDirectoryA(directoryPath.c_str()) != FALSE;
	}

	bool ReadTextFile(const std::string& path, std::string& outText)
	{
		std::ifstream file(path, std::ios::in | std::ios::binary);
		if (!file.is_open())
		{
			return false;
		}

		outText.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		return true;
	}

	std::string NormalizeResourceKey(const std::string& resourceKey)
	{
		std::string normalized = resourceKey;
		std::replace(normalized.begin(), normalized.end(), '/', '\\');
		return normalized;
	}

	std::string ToLowerCopy(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
		{
			return static_cast<char>(std::tolower(ch));
		});
		return value;
	}

	bool EndsWithLowercase(const std::string& value, const char* suffix)
	{
		const size_t suffixLength = strlen(suffix);
		return value.length() >= suffixLength
			&& value.compare(value.length() - suffixLength, suffixLength, suffix) == 0;
	}

	bool IsSoundResourceKey(const std::string& resourceKey)
	{
		const std::string lowered = ToLowerCopy(resourceKey);
		return EndsWithLowercase(lowered, ".wav")
			|| EndsWithLowercase(lowered, ".mp3")
			|| EndsWithLowercase(lowered, ".ogg");
	}

	void CollectJsonStringValues(const std::string& jsonText, const char* fieldName, std::set<std::string>& outValues)
	{
		const std::regex pattern(std::string("\"") + fieldName + "\"\\s*:\\s*\"([^\"]*)\"");
		for (std::sregex_iterator itr(jsonText.begin(), jsonText.end(), pattern), end; itr != end; ++itr)
		{
			const std::string value = (*itr)[1].str();
			if (!value.empty())
			{
				outValues.insert(value);
			}
		}
	}

	void CollectJsonSoundLikeStringValues(const std::string& jsonText, std::set<std::string>& outValues)
	{
		const std::regex pattern("\"([^\"]+)\"");
		for (std::sregex_iterator itr(jsonText.begin(), jsonText.end(), pattern), end; itr != end; ++itr)
		{
			const std::string value = (*itr)[1].str();
			if (value.empty() || !IsSoundResourceKey(value))
			{
				continue;
			}

			outValues.insert(value);
		}
	}

	bool CopySceneReferencedResources(
		const std::vector<std::string>& sceneNames,
		const std::string& sourceResourcesPath,
		const std::string& buildResourcesPath,
		std::string& outErrorMessage)
	{
		(void)sceneNames;

		if (DeleteDirectoryRecursive(buildResourcesPath) == false && DirectoryExists(buildResourcesPath))
		{
			outErrorMessage = "기존 BuildOutput\\Resources 정리에 실패했습니다.";
			return false;
		}

		if (!DirectoryExists(sourceResourcesPath))
		{
			outErrorMessage = "원본 Resources 폴더를 찾지 못했습니다.";
			return false;
		}

		if (!CopyDirectoryRecursive(sourceResourcesPath, buildResourcesPath))
		{
			outErrorMessage = "Resources 폴더 전체 복사에 실패했습니다.";
			return false;
		}

		return true;
	}

	bool TryRunBuildProcess(const std::string& applicationPath, const std::string& commandLine, const std::string& workingDirectory, DWORD& outExitCode)
	{
		STARTUPINFOA startupInfo = {};
		startupInfo.cb = sizeof(startupInfo);
		PROCESS_INFORMATION processInfo = {};

		std::vector<char> mutableCommandLine(commandLine.begin(), commandLine.end());
		mutableCommandLine.push_back('\0');

		if (!CreateProcessA(
			applicationPath.empty() ? nullptr : applicationPath.c_str(),
			mutableCommandLine.data(),
			nullptr,
			nullptr,
			FALSE,
			CREATE_NO_WINDOW,
			nullptr,
			workingDirectory.c_str(),
			&startupInfo,
			&processInfo))
		{
			return false;
		}

		WaitForSingleObject(processInfo.hProcess, INFINITE);

		outExitCode = 1;
		GetExitCodeProcess(processInfo.hProcess, &outExitCode);
		CloseHandle(processInfo.hThread);
		CloseHandle(processInfo.hProcess);

		return true;
	}

	bool RunReleaseGameAppBuild(const std::string& solutionRoot)
	{
		SetBuildGameStage(1, 6, "1/6 Release|x64 게임 빌드를 실행하는 중...");

		const std::string projectMsbuildScriptPath = solutionRoot + "\\Tools\\MSBuild\\msbuild.cmd";
		const std::string solutionPath = solutionRoot + "\\Solution_Kirby.sln";
		const std::string msbuildArguments = "\"" + solutionPath + "\" /t:GameApp /p:Configuration=Release /p:Platform=x64 /m:1 /p:UseMultiToolTask=false /p:CL_MPCount=1";
		char comSpecBuffer[MAX_PATH] = { 0 };
		const DWORD comSpecLength = GetEnvironmentVariableA("ComSpec", comSpecBuffer, MAX_PATH);
		const std::string comSpecPath =
			(comSpecLength > 0 && comSpecLength < MAX_PATH)
			? std::string(comSpecBuffer)
			: std::string("C:\\Windows\\System32\\cmd.exe");
		const std::string scriptCommandLine =
			"cmd.exe /S /C \"call \"" + projectMsbuildScriptPath + "\" " + msbuildArguments + "\"";
		DWORD exitCode = 1;
		if (!TryRunBuildProcess(comSpecPath, scriptCommandLine, solutionRoot, exitCode))
		{
			SetBuildGameResult("프로젝트 MSBuild 실행 스크립트를 시작하지 못했습니다: " + comSpecPath);
			return false;
		}

		if (exitCode != 0)
		{
			if (exitCode == 9009)
			{
				SetBuildGameResult("MSBuild를 찾지 못했습니다. Visual Studio 또는 Visual Studio Build Tools를 설치한 뒤 다시 실행해 주세요.");
				return false;
			}

			SetBuildGameResult(
				"Release|x64 게임 빌드에 실패했습니다. exitCode=" + std::to_string(static_cast<unsigned long long>(exitCode)) +
				", script=" + projectMsbuildScriptPath);
			return false;
		}

		return true;
	}

	bool BuildGamePackage(const BuildSettingsData& sourceSettings)
	{
		BuildSettingsData settings = sourceSettings;
		if (!BuildSettingsManager::Validate(settings))
		{
			SetBuildGameResult("BuildSettings가 올바르지 않아 빌드를 진행할 수 없습니다.");
			return false;
		}

		std::vector<std::string> sceneNames = settings.scenes;
		bool startSceneIncluded = false;
		for (std::vector<std::string>::const_iterator itr = sceneNames.begin(); itr != sceneNames.end(); ++itr)
		{
			if (*itr == settings.startScene)
			{
				startSceneIncluded = true;
				break;
			}
		}

		if (!startSceneIncluded)
		{
			SetBuildGameResult("startScene은 빌드 대상 scenes에 포함되어 있어야 합니다.");
			return false;
		}

		const std::string buildSettingsPath = BuildSettingsManager::GetBuildSettingsPath();
		const size_t fileNamePos = buildSettingsPath.find_last_of("\\/");
		const std::string solutionRoot = fileNamePos == std::string::npos ? std::string(".") : buildSettingsPath.substr(0, fileNamePos);
		const std::string buildOutputPath = solutionRoot + "\\BuildOutput";
		const std::string buildSceneDataPath = buildOutputPath + "\\SceneData";
		const std::string buildResourcesPath = buildOutputPath + "\\Resources";
		const std::string outputBuildSettingsPath = buildOutputPath + "\\BuildSettings.json";
		const std::string releaseOutputPath = solutionRoot + "\\Bin\\Release_x64";
		const std::string sourceExePath = releaseOutputPath + "\\GameApp.exe";
		const std::string sourceGameDllPath = releaseOutputPath + "\\KirbyGameDll.dll";
		const std::string sourceResourcesPath = solutionRoot + "\\Resources";

		if (!RunReleaseGameAppBuild(solutionRoot))
		{
			return false;
		}

		SetBuildGameStage(2, 6, "2/6 BuildOutput 폴더를 준비하는 중...");
		if (CreateDirectoryA(buildOutputPath.c_str(), nullptr) == FALSE && GetLastError() != ERROR_ALREADY_EXISTS)
		{
			SetBuildGameResult("BuildOutput 폴더 생성에 실패했습니다.");
			return false;
		}

		if (DeleteDirectoryRecursive(buildSceneDataPath) == false && DirectoryExists(buildSceneDataPath))
		{
			SetBuildGameResult("기존 BuildOutput\\SceneData 정리에 실패했습니다.");
			return false;
		}

		if (CreateDirectoryA(buildSceneDataPath.c_str(), nullptr) == FALSE && GetLastError() != ERROR_ALREADY_EXISTS)
		{
			SetBuildGameResult("BuildOutput\\SceneData 폴더 생성에 실패했습니다.");
			return false;
		}

		if (!FileExists(sourceExePath))
		{
			SetBuildGameResult("Release|x64 출력에서 GameApp.exe를 찾지 못했습니다.");
			return false;
		}

		if (!FileExists(sourceGameDllPath))
		{
			SetBuildGameResult("Release|x64 출력에서 KirbyGameDll.dll을 찾지 못했습니다.");
			return false;
		}

		SetBuildGameStage(3, 6, "3/6 빌드 대상 SceneData를 복사하는 중...");
		for (std::vector<std::string>::const_iterator itr = sceneNames.begin(); itr != sceneNames.end(); ++itr)
		{
			if (!SceneDataManager::Exists(*itr))
			{
				SetBuildGameResult("빠진 SceneData가 있어 빌드를 중단했습니다: " + *itr);
				return false;
			}

			const std::string sourcePath = SceneDataManager::GetSceneDataPath(*itr);
			const std::string destinationPath = buildSceneDataPath + "\\" + *itr + ".json";
			if (CopyFileA(sourcePath.c_str(), destinationPath.c_str(), FALSE) == FALSE)
			{
				SetBuildGameResult("SceneData 복사에 실패했습니다: " + *itr);
				return false;
			}
		}

		SetBuildGameStage(4, 6, "4/6 BuildSettings.json을 생성하는 중...");
		if (!BuildSettingsManager::SaveToPath(settings, outputBuildSettingsPath))
		{
			SetBuildGameResult("BuildOutput에 BuildSettings.json 생성에 실패했습니다.");
			return false;
		}

		SetBuildGameStage(5, 6, "5/6 Release 실행 파일과 DLL을 복사하는 중...");
		if (!CopySingleFile(sourceExePath, buildOutputPath + "\\GameApp.exe"))
		{
			SetBuildGameResult("GameApp.exe 복사에 실패했습니다.");
			return false;
		}

		WIN32_FIND_DATAA dllFindData = {};
		HANDLE dllFindHandle = FindFirstFileA((releaseOutputPath + "\\*.dll").c_str(), &dllFindData);
		if (dllFindHandle == INVALID_HANDLE_VALUE)
		{
			SetBuildGameResult("Release 출력 폴더의 DLL 목록을 읽지 못했습니다.");
			return false;
		}

		do
		{
			if ((dllFindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			{
				continue;
			}

			const std::string dllName = dllFindData.cFileName;
			const std::string sourceDllPath = releaseOutputPath + "\\" + dllName;
			if (!CopySingleFile(sourceDllPath, buildOutputPath + "\\" + dllName))
			{
				FindClose(dllFindHandle);
				SetBuildGameResult("DLL 복사에 실패했습니다: " + dllName);
				return false;
			}
		}
		while (FindNextFileA(dllFindHandle, &dllFindData) != FALSE);

		FindClose(dllFindHandle);

		SetBuildGameStage(6, 6, "6/6 Resources 복사하는 중...");
		std::string resourceCopyErrorMessage;
		if (!CopySceneReferencedResources(sceneNames, sourceResourcesPath, buildResourcesPath, resourceCopyErrorMessage))
		{
			SetBuildGameResult(resourceCopyErrorMessage);
			return false;
		}

		SetBuildGameResult("BuildOutput 생성과 SceneData/BuildSettings, Release exe/dll/Resources 복사가 완료되었습니다.");
		return true;
	}

	void RequestBuildGame()
	{
		if (IsBuildGameRunning())
		{
			return;
		}

		if (ObjectManager::GetInstance() != nullptr)
		{
			ObjectManager::GetInstance()->FlushPendingObjects();
		}

		const std::string currentSceneName = GetCurrentSceneName();
		if (!currentSceneName.empty() && EditorSceneWorkflow::IsSceneDirty(currentSceneName))
		{
			if (!EditorSceneWorkflow::SaveCurrentSceneDataWithBaseline(currentSceneName))
			{
				SetBuildGameResult("현재 씬 저장에 실패해서 빌드를 시작하지 않습니다.");
				return;
			}
		}

		BuildSettingsManager::Validate(g_buildSettingsUiState);
		if (!BuildSettingsManager::Save(g_buildSettingsUiState))
		{
			SetBuildGameResult("BuildSettings 저장에 실패해서 빌드를 시작하지 않습니다.");
			return;
		}

		const BuildSettingsData settings = g_buildSettingsUiState;
		BeginBuildGameAsync(6, "빌드 작업을 준비하는 중...");
		g_buildGameFuture = std::async(std::launch::async, [settings]()
		{
			if (!BuildGamePackage(settings))
			{
				BuildGameUiSnapshot snapshot = GetBuildGameUiSnapshot();
				if (snapshot.resultMessage.empty())
				{
					SetBuildGameResult("빌드 시작에 실패했습니다.");
				}
			}
		});
	}
}

void EditorBuildSettingsPanel::Draw()
{
	PollBuildGameAsync();
	EnsureBuildSettingsUiLoaded();

	if (!ImGui::CollapsingHeader("Build Settings", ImGuiTreeNodeFlags_DefaultOpen))
	{
		return;
	}

	const std::vector<std::string> sceneNames = SceneDataManager::GetSceneFileList();
	if (sceneNames.empty())
	{
		ImGui::TextDisabled("SceneData JSON not found.");
		return;
	}

	std::string currentSceneName;
	const bool canUseCurrentScene = CanUseCurrentSceneForBuildSettings(&currentSceneName);
	const bool currentSceneAlreadyIncluded = !currentSceneName.empty() && IsSceneIncludedInBuildSettings(currentSceneName);

	ImGui::Text("Scenes");
	for (std::vector<std::string>::const_iterator itr = sceneNames.begin(); itr != sceneNames.end(); ++itr)
	{
		bool included = IsSceneIncludedInBuildSettings(*itr);
		if (ImGui::Checkbox(itr->c_str(), &included))
		{
			if (included)
			{
				IncludeSceneInBuildSettings(*itr);
			}
			else
			{
				ExcludeSceneFromBuildSettings(*itr);
			}
		}
	}

	BuildSettingsManager::Validate(g_buildSettingsUiState);
	BuildSettingsValidationState validation = ValidateBuildSettingsUiState(g_buildSettingsUiState);

	ImGui::Separator();
	ImGui::Text("Current Scene");

	if (currentSceneName.empty())
	{
		ImGui::TextDisabled("Current scene name is empty.");
	}
	else if (!canUseCurrentScene)
	{
		ImGui::TextDisabled("Save the current scene first before adding it.");
	}
	else
	{
		ImGui::Text("%s", currentSceneName.c_str());
	}

	if (!canUseCurrentScene || currentSceneAlreadyIncluded)
	{
		ImGui::BeginDisabled();
	}

	if (ImGui::Button("Add Current Scene") && canUseCurrentScene && !currentSceneAlreadyIncluded)
	{
		IncludeSceneInBuildSettings(currentSceneName);
		validation = ValidateBuildSettingsUiState(g_buildSettingsUiState);
	}

	if (!canUseCurrentScene || currentSceneAlreadyIncluded)
	{
		ImGui::EndDisabled();
	}

	ImGui::SameLine();
	if (!canUseCurrentScene || !currentSceneAlreadyIncluded)
	{
		ImGui::BeginDisabled();
	}

	if (ImGui::Button("Set Current As Start") && canUseCurrentScene && currentSceneAlreadyIncluded)
	{
		g_buildSettingsUiState.startScene = currentSceneName;
		validation = ValidateBuildSettingsUiState(g_buildSettingsUiState);
	}

	if (!canUseCurrentScene || !currentSceneAlreadyIncluded)
	{
		ImGui::EndDisabled();
	}

	if (currentSceneAlreadyIncluded)
	{
		ImGui::TextDisabled("Current scene is already included.");
	}

	ImGui::Separator();
	ImGui::Text("Start Scene");

	if (g_buildSettingsUiState.scenes.empty())
	{
		ImGui::TextDisabled("Select at least one scene.");
	}
	else
	{
		const char* preview = g_buildSettingsUiState.startScene.empty()
			? g_buildSettingsUiState.scenes.front().c_str()
			: g_buildSettingsUiState.startScene.c_str();

		if (ImGui::BeginCombo("##BuildStartScene", preview))
		{
			for (std::vector<std::string>::const_iterator itr = g_buildSettingsUiState.scenes.begin(); itr != g_buildSettingsUiState.scenes.end(); ++itr)
			{
				const bool isSelected = (*itr == g_buildSettingsUiState.startScene);
				if (ImGui::Selectable(itr->c_str(), isSelected))
				{
					g_buildSettingsUiState.startScene = *itr;
				}

				if (isSelected)
				{
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
	}

	validation = ValidateBuildSettingsUiState(g_buildSettingsUiState);

	ImGui::Separator();
	ImGui::Text("Validation");

	if (validation.hasEmptyScenes)
	{
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Build scenes is empty.");
	}
	if (validation.hasEmptyStartScene)
	{
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Start scene is empty.");
	}
	if (validation.hasStartSceneOutsideScenes)
	{
		ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Start scene must be included in scenes.");
	}
	if (validation.hasMissingSceneFile)
	{
		for (std::vector<std::string>::const_iterator itr = validation.missingSceneNames.begin(); itr != validation.missingSceneNames.end(); ++itr)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Missing SceneData: %s", itr->c_str());
		}
	}
	if (!validation.hasBlockingError)
	{
		ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.35f, 1.0f), "Build settings is valid.");
	}

	const BuildGameUiSnapshot buildGameSnapshot = GetBuildGameUiSnapshot();
	if (buildGameSnapshot.running)
	{
		const float progress = buildGameSnapshot.totalStages > 0
			? static_cast<float>(buildGameSnapshot.currentStage) / static_cast<float>(buildGameSnapshot.totalStages)
			: 0.0f;
		ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));
		ImGui::TextWrapped("%s", buildGameSnapshot.stageMessage.c_str());
	}

	if (validation.hasBlockingError)
	{
		ImGui::BeginDisabled();
	}
	else if (buildGameSnapshot.running)
	{
		ImGui::BeginDisabled();
	}

	if (ImGui::Button("Save Build Settings"))
	{
		BuildSettingsManager::Validate(g_buildSettingsUiState);
		const bool saved = BuildSettingsManager::Save(g_buildSettingsUiState);
		std::cout << (saved ? "Save Build Settings succeeded" : "Save Build Settings failed") << std::endl;
		if (saved)
		{
			g_buildSettingsUiLoaded = false;
			g_buildGameStatusMessage = "BuildSettings 저장이 완료되었습니다.";
		}
		else
		{
			g_buildGameStatusMessage = "BuildSettings 저장에 실패했습니다.";
		}
	}

	ImGui::SameLine();
	if (ImGui::Button("Build Game"))
	{
		RequestBuildGame();
	}

	if (validation.hasBlockingError || buildGameSnapshot.running)
	{
		ImGui::EndDisabled();
		if (validation.hasBlockingError)
		{
			ImGui::TextDisabled("Fix validation errors before saving.");
		}
	}

	if (!buildGameSnapshot.resultMessage.empty())
	{
		ImGui::TextWrapped("%s", buildGameSnapshot.resultMessage.c_str());
	}
}
