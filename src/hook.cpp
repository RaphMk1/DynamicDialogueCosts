#include "Hook.h"
#include "PCH.h"
#include "dialogueRegistry.h"
#include "settings.h"

namespace DynamicDialogueCosts
{
	extern RE::ObjectRefHandle g_targetSpeakerHandle;
	extern std::atomic<bool>   g_isDialogueScaled;

	bool CanActorTalkToPlayer(RE::Actor* actor);
	void CacheSpeakerModifiers(RE::Actor* merchant = nullptr);

	namespace Hooks
	{
		class DialogueMenuHook
		{
		public:
			static void Install()
			{
				REL::Relocation<std::uintptr_t> dgVtable{ RE::DialogueMenu::VTABLE[0] };
				_ProcessMessage = dgVtable.write_vfunc(0x4, ProcessMessage);
				SKSE::log::warn("[HOOK] Hooked Dialogue Menu's PM");
			}

		private:
			static RE::UI_MESSAGE_RESULTS ProcessMessage(RE::DialogueMenu* a_this, RE::UIMessage& a_message)
			{
				if (a_message.type == RE::UI_MESSAGE_TYPE::kShow) {
					RE::Actor*          currentSpeaker = nullptr;
					RE::ObjectRefHandle currentHandle{};
					auto                topicManager = RE::MenuTopicManager::GetSingleton();
					auto                cfg = g_settings.load();

					if (topicManager && topicManager->speaker) {
						if (auto smartPtr = topicManager->speaker.get()) {
							currentSpeaker = smartPtr->As<RE::Actor>();
							currentHandle = topicManager->speaker;
						}
					}

					if (!currentSpeaker && cfg && cfg->useActivateHandler) {
						if (auto handlePtr = g_targetSpeakerHandle.get()) {
							currentSpeaker = handlePtr->As<RE::Actor>();
							currentHandle = g_targetSpeakerHandle;
						}
					}

					if (!currentSpeaker && topicManager && topicManager->lastSpeaker) {
						if (auto smartPtr = topicManager->lastSpeaker.get()) {
							currentSpeaker = smartPtr->As<RE::Actor>();
							currentHandle = topicManager->lastSpeaker;
						}
					}

					if (g_isDialogueScaled.load()) {
						DialoguePriceTransformer::ResetGlobalsToBaseline();
					}

					if (currentSpeaker && CanActorTalkToPlayer(currentSpeaker)) {
						SKSE::log::trace("[DGSS] Speaker validated: 0x{:08X}. CacheSpeakerModifiers starting...", currentSpeaker->GetFormID());
						g_targetSpeakerHandle = currentHandle;
						CacheSpeakerModifiers(currentSpeaker);

						DialoguePriceTransformer::ResetPersistentToBaseline();

						SKSE::log::trace("[DGSS] Calling ApplySettModifiers...");
						DialoguePriceTransformer::ApplySettModifiers();

						SKSE::log::trace("[DGSS] Calling ApplyActiveGlobalModifiers...");
						DialoguePriceTransformer::ApplyActiveGlobalModifiers();

						if (topicManager) {
							using func_t = void();
							static REL::Relocation<func_t> SkipTextCallback{ RELOCATION_ID(50614, 51508) };
							SKSE::log::debug("[DGSS] Attempting dialogue delay skip for: 0x{:08X}", currentSpeaker->GetFormID());
							SkipTextCallback();
						}
					} else {
						SKSE::log::warn("[DGSS] Dialogue Menu opened without valid speaker actor.");
					}
				}

				return _ProcessMessage(a_this, a_message);
			}

			static inline REL::Relocation<decltype(&ProcessMessage)> _ProcessMessage;
		};

		void Install()
		{
			DialogueMenuHook::Install();
		}
	}
}