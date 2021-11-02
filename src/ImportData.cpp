#include "ImportData.h"
#include "ActionGenerator.h"
#include "Settings.h"
#include "TagFactory.h"

void ImportData::InsertCustomIcons(
	const std::vector<IconInfo>& a_iconInfo,
	std::size_t a_insertPos,
	std::size_t a_undiscoveredOffset,
	std::size_t a_baseCount)
{
	auto settings = Settings::GetSingleton();

	bool obscureUndiscovered = false;
	if (_menuType == MenuType::HUD) {
		obscureUndiscovered = settings->HUD.bObscuredUndiscovered;
	}
	else if (_menuType == MenuType::Map) {
		obscureUndiscovered = settings->Map.bObscuredUndiscovered;
	}

	const std::string& resourceFile = settings->Resources.sResourceFile;
	auto resourcePath = std::filesystem::path{ "Data\\MapMarkers"sv } / resourceFile;
	auto resourcePathStr = resourcePath.string();

	_numIcons = a_iconInfo.size();
	_iconScales.reserve(_numIcons);

	for (std::size_t i = 0; i < _numIcons; i++) {
		auto& iconInfo = a_iconInfo[i];
		auto [it, result] = _importResources.try_emplace(iconInfo.SourcePath);
		it->second.push_back({ iconInfo.ExportName, IconTypes::Discovered, i });
		if (!obscureUndiscovered) {
			it->second.push_back({ iconInfo.ExportNameUndiscovered, IconTypes::Undiscovered, i });
		}

		_iconScales.push_back(iconInfo.IconScale);
	}

	if (obscureUndiscovered) {
		auto [it, result] = _importResources.try_emplace(resourcePathStr);
		it->second.push_back({ "ObscuredUndiscoveredMarker"s, IconTypes::Undiscovered, 0 });
	}

	for (auto& [sourcePath, iconIds] : _importResources)
	{
		_importedMovies[sourcePath] = LoadMovie(sourcePath);
	}

	LoadIcons();
	ImportMovies();
	ImportResources();

	std::int32_t newFrames = static_cast<std::int32_t>(_numIcons);

	_marker->frames.InsertMultipleAt(a_insertPos, _numIcons);
	auto undiscoveredOffset = a_undiscoveredOffset + _numIcons;

	_marker->frames.InsertMultipleAt(a_insertPos + undiscoveredOffset, _numIcons);

	_marker->frameCount += newFrames * 2;
	_marker->frameLoading += newFrames * 2;

	auto& loadTaskData = _marker->movieData->loadTaskData;
	auto& allocator = loadTaskData->allocator;
	auto alloc = [&allocator](std::size_t a_size) { return allocator.Alloc(a_size); };

	for (std::int32_t iconType = 0; iconType < IconTypes::Total; iconType++) {

		if (obscureUndiscovered && iconType == IconTypes::Undiscovered) {
			continue;
		}

		auto insertPos = a_insertPos;
		if (iconType == IconTypes::Undiscovered) {
			insertPos += undiscoveredOffset;
		}

		for (std::int32_t i = 0; i < _numIcons; i++) {

			auto placeObject = MakeReplaceObject(alloc, _ids[iconType][i]);
			assert(placeObject);

			if (_menuType == MenuType::Map && _iconScales[i] != 1.0) {
				auto doAction = MakeMarkerScaleAction(alloc, _iconScales[i]);
				assert(doAction);

				_marker->frames[i + insertPos] = MakeTagList(alloc, { placeObject, doAction });
			}
			else {
				_marker->frames[i + insertPos] = MakeTagList(alloc, { placeObject });
			}
		}
	}

	if (obscureUndiscovered) {
		auto placeObject = MakeReplaceObject(alloc, _ids[IconTypes::Undiscovered][0]);
		auto start = a_insertPos + undiscoveredOffset - a_baseCount;
		_marker->frames[start] = MakeTagList(alloc, { placeObject });

		for (std::int32_t i = 1; i < a_baseCount; i++) {
			_marker->frames[start + i] = { nullptr, 0 };
		}
	}
}

auto ImportData::LoadMovie(const std::string& a_sourcePath) -> RE::GFxMovieDefImpl*
{
	auto scaleformManager = RE::BSScaleformManager::GetSingleton();
	auto loader = scaleformManager->loader;
	auto movieDef = loader->CreateMovie(a_sourcePath.data());
	if (!movieDef) {
		logger::error("Failed to create movie from {}"sv, a_sourcePath);
		return nullptr;
	}

	static REL::Relocation<std::uintptr_t> GFxMovieDefImpl_vtbl{ REL::ID(562342), 0x4BF0 };
	if (*reinterpret_cast<std::uintptr_t*>(movieDef) != GFxMovieDefImpl_vtbl.get()) {
		logger::critical("Loaded movie did not have the expected virtual table, aborting"sv);
		return nullptr;
	}

	auto movieDefImpl = static_cast<RE::GFxMovieDefImpl*>(movieDef);

	_importedMovies[a_sourcePath] = movieDefImpl;

	return movieDefImpl;
}

void ImportData::LoadIcons()
{
	using ResourceType = RE::GFxResource::ResourceType;

	_icons[IconTypes::Discovered].resize(_numIcons);
	_icons[IconTypes::Undiscovered].resize(_numIcons);

	for (auto& [sourcePath, iconIds] : _importResources) {
		auto& movie = _importedMovies.at(sourcePath);

		for (auto& iconId : iconIds) {
			auto icon = movie->GetResource(iconId.exportName.data());
			if (icon && icon->GetResourceType() == ResourceType::kSpriteDef) {
				_icons[iconId.iconType][iconId.index] = static_cast<RE::GFxSpriteDef*>(icon);
			}
		}
	}
}

void ImportData::ImportMovies()
{
	auto& bindTaskData = _targetMovie->bindTaskData;
	auto& loadTaskData = bindTaskData->movieDataResource->loadTaskData;

	auto& allocator = loadTaskData->allocator;
	auto alloc = [&allocator](std::size_t a_size) { return allocator.Alloc(a_size); };

	auto& importedMovies = bindTaskData->importedMovies;
	std::size_t numImportedMovies = importedMovies.GetSize();
	importedMovies.Reserve(numImportedMovies + _importedMovies.size());

	for (auto& [path, movie] : _importedMovies) {
		auto movieIndex = importedMovies.GetSize();
		_movieIndices[path] = static_cast<std::uint32_t>(movieIndex);
		importedMovies.PushBack(movie);
	}

	std::size_t arraySize = sizeof(RE::GASExecuteTag*) * _importedMovies.size();
	auto tagList = static_cast<RE::GASExecuteTag**>(allocator.Alloc(arraySize));
	for (std::size_t i = 0, size = _importedMovies.size(); i < size; i++) {
		auto movie = numImportedMovies + i;
		tagList[i] = TagFactory::MakeInitImportActions(
			alloc,
			static_cast<std::uint32_t>(movie));
	}

	loadTaskData->importFrames.PushBack(ExecuteTagList{
		.data = tagList,
		.size = static_cast<std::uint32_t>(_importedMovies.size()),
	});

	loadTaskData->importFrameCount++;
}

void ImportData::ImportResources()
{
	using ImportedResource = RE::GFxMovieDefImpl::ImportedResource;

	auto& bindTaskData = _targetMovie->bindTaskData;
	auto& importData = bindTaskData->importData;

	auto& loadTaskData = bindTaskData->movieDataResource->loadTaskData;
	auto& resources = loadTaskData->resources;

	auto& allocator = loadTaskData->allocator;

	std::uint16_t nextId = 0;
	std::vector<ImportedResource> newResources;

	_ids[IconTypes::Discovered].resize(_numIcons);
	_ids[IconTypes::Undiscovered].resize(_numIcons);

	for (auto& [sourcePath, iconIds] : _importResources) {
		auto& movie = _importedMovies.at(sourcePath);
		std::uint32_t movieIndex = _movieIndices.at(sourcePath);

		auto importInfo = new (allocator.Alloc(sizeof(RE::GFxImportNode))) RE::GFxImportNode{
			.filename = sourcePath.data(),
			.frame = static_cast<std::uint32_t>(loadTaskData->importFrames.GetSize()),
			.movieIndex = movieIndex,
		};

		for (auto& iconId : iconIds) {
			RE::GFxResourceID resourceId;
			do {
				resourceId = RE::GFxResourceID{ ++nextId };
			} while (resources.Find(resourceId) != resources.end());

			_ids[iconId.iconType][iconId.index] = nextId;

			RE::GFxResourceSource resourceSource{};
			resourceSource.type = RE::GFxResourceSource::kImported;
			resourceSource.data.importSource.index = loadTaskData->importedResourceCount;

			loadTaskData->importedResourceCount++;
			loadTaskData->resources.Add(resourceId, resourceSource);

			RE::GFxImportNode::ImportAssetInfo assetInfo{
				.name = RE::GString{ iconId.exportName.data() },
				.id = nextId,
				.importIndex = resourceSource.data.importSource.index,
			};

			importInfo->assets.PushBack(assetInfo);

			ImportedResource importedResource{};
			importedResource.importData = std::addressof(movie->bindTaskData->importData);
			importedResource.resource = RE::GPtr(_icons[iconId.iconType][iconId.index]);
			newResources.push_back(importedResource);
		}

		if (loadTaskData->importInfoBegin) {
			auto& prev = loadTaskData->importInfoEnd;
			prev->nextInChain = importInfo;
		}
		else {
			loadTaskData->importInfoBegin = importInfo;
		}

		loadTaskData->importInfoEnd = importInfo;
	}

	std::uint32_t newCount = (loadTaskData->importedResourceCount + 0xF) & -0x10;

	if (newCount > importData.importCount) {
		std::size_t newSize = newCount * sizeof(ImportedResource);
		auto newArray = new (importData.heap->Alloc(newSize, 0)) ImportedResource[newCount];

		std::copy_n(importData.resourceArray, importData.importCount, newArray);

		for (std::uint32_t i = 0; i < importData.importCount; i++) {
			importData.resourceArray[i].resource->Release();
		}

		importData.resourceArray = newArray;
		importData.importCount = newCount;
	}

	std::uint32_t start =
		loadTaskData->importedResourceCount - static_cast<std::uint32_t>(newResources.size());

	std::copy(
		newResources.begin(),
		newResources.end(),
		std::addressof(importData.resourceArray[start]));
}

auto ImportData::MakeReplaceObject(AllocateCallback a_alloc, std::uint16_t a_characterId)
	-> RE::GFxPlaceObjectBase*
{
	RE::GFxPlaceObjectData placeObjectData{};
	placeObjectData.placeFlags.set(
		RE::GFxPlaceFlags::kMove,
		RE::GFxPlaceFlags::kHasCharacter,
		RE::GFxPlaceFlags::kHasMatrix);
	placeObjectData.depth = 1;
	placeObjectData.characterId = RE::GFxResourceID{ a_characterId };
	placeObjectData.matrix.SetMatrix(0.8f, 0.0f, 0.0f, 0.8f, 0.0f, 0.0f);

	return TagFactory::MakePlaceObject(a_alloc, placeObjectData);
}

auto ImportData::MakeMarkerScaleAction(AllocateCallback a_alloc, float a_iconScale)
	-> RE::GASDoAction*
{
	struct Action : ActionGenerator
	{
		Action(float a_iconScale)
		{
			// var marker = this._parent._parent._parent;
			Push("marker");
			Push("this");
			GetVariable();
			Push("_parent");
			GetMember();
			Push("_parent");
			GetMember();
			Push("_parent");
			GetMember();
			DefineLocal();

			// marker._width *= a_iconScale;
			Push("marker");
			GetVariable();
			Push("_width");
			Push("marker");
			GetVariable();
			Push("_width");
			GetMember();
			Push(a_iconScale);
			Multiply();
			SetMember();

			// marker._height *= a_iconScale;
			Push("marker");
			GetVariable();
			Push("_height");
			Push("marker");
			GetVariable();
			Push("_height");
			GetMember();
			Push(a_iconScale);
			Multiply();
			SetMember();
		}
	};
	Action action{ a_iconScale };
	action.Ready();
	auto bufferData = action.GetCode();

	return TagFactory::MakeDoAction(a_alloc, bufferData);
}

auto ImportData::MakeTagList(
	AllocateCallback a_alloc,
	std::initializer_list<RE::GASExecuteTag*> a_tags) -> ExecuteTagList
{
	std::size_t size = sizeof(RE::GASExecuteTag*) * a_tags.size();
	auto tagArray = static_cast<RE::GASExecuteTag**>(a_alloc(size));

	std::copy(a_tags.begin(), a_tags.end(), tagArray);

	return ExecuteTagList{
		.data = tagArray,
		.size = static_cast<std::uint32_t>(a_tags.size()),
	};
}