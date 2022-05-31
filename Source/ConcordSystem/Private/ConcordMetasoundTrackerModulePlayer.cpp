// Copyright Jan Klimaschewski. All Rights Reserved.

#include "ConcordMetasoundTrackerModulePlayer.h"
#include "MetasoundLog.h"

using namespace Metasound;

TUniquePtr<IOperator> FConcordTrackerModulePlayerNode::FOperatorFactory::CreateOperator(const FCreateOperatorParams& InParams, 
                                                                          FBuildErrorArray& OutErrors) 
{
    const FConcordTrackerModulePlayerNode& Node = static_cast<const FConcordTrackerModulePlayerNode&>(InParams.Node);
    const FDataReferenceCollection& Inputs = InParams.InputDataReferences;
    const FInputVertexInterface& InputInterface = DeclareVertexInterface().GetInputInterface();

    return MakeUnique<FConcordTrackerModulePlayerOperator>(InParams.OperatorSettings,
                                                           Inputs.GetDataReadReferenceOrConstruct<FConcordMetasoundTrackerModuleAsset>("Tracker Module"),
                                                           Inputs.GetDataReadReferenceOrConstruct<FConcordMetasoundPatternAsset>("Pattern"),
                                                           Inputs.GetDataReadReferenceOrConstruct<FTrigger>("Start", InParams.OperatorSettings),
                                                           Inputs.GetDataReadReferenceOrConstruct<FTrigger>("Stop", InParams.OperatorSettings),
                                                           Inputs.GetDataReadReferenceOrConstructWithVertexDefault<int32>(InputInterface, "Start Line", InParams.OperatorSettings),
                                                           Inputs.GetDataReadReferenceOrConstructWithVertexDefault<bool>(InputInterface, "Loop", InParams.OperatorSettings));
}

const FVertexInterface& FConcordTrackerModulePlayerNode::DeclareVertexInterface()
{
    static const FVertexInterface VertexInterface(FInputVertexInterface(TInputDataVertexModel<FConcordMetasoundTrackerModuleAsset>("Tracker Module", INVTEXT("The tracker module to play.")),
                                                                        TInputDataVertexModel<FConcordMetasoundPatternAsset>("Pattern", INVTEXT("The pattern to play.")),
                                                                        TInputDataVertexModel<FTrigger>("Start", INVTEXT("Start the Player.")),
                                                                        TInputDataVertexModel<FTrigger>("Stop", INVTEXT("Stop the Player.")),
                                                                        TInputDataVertexModel<int32>("Start Line", INVTEXT("The line to start the Player at."), 0),
                                                                        TInputDataVertexModel<bool>("Loop", INVTEXT("Loop the Player instead of stopping when finished."), true)),
                                                  FOutputVertexInterface(TOutputDataVertexModel<FAudioBuffer>("Out Left", INVTEXT("Left Audio Output")),
                                                                         TOutputDataVertexModel<FAudioBuffer>("Out Right", INVTEXT("Right Audio Output"))));
    return VertexInterface;
}

const FNodeClassMetadata& FConcordTrackerModulePlayerNode::GetNodeInfo()
{
    auto InitNodeInfo = []() -> FNodeClassMetadata
    {
        FNodeClassMetadata Info;
        Info.ClassName = { "Concord", "Tracker Module Player", "Default" };
        Info.MajorVersion = 1;
        Info.MinorVersion = 0;
        Info.DisplayName = INVTEXT("Concord Tracker Module Player");
        Info.Description = INVTEXT("Plays back an Impulse Tracker Module with Concord Pattern information.");
        Info.Author = TEXT("Jan Klimaschewski");
        Info.PromptIfMissing = INVTEXT("Missing :(");
        Info.DefaultInterface = DeclareVertexInterface();

        return Info;
    };

    static const FNodeClassMetadata Info = InitNodeInfo();

    return Info;
}

FConcordTrackerModulePlayerNode::FConcordTrackerModulePlayerNode(const FVertexName& InName, const FGuid& InInstanceID)
    :	FNode(InName, InInstanceID, GetNodeInfo())
    ,	Factory(MakeOperatorFactoryRef<FConcordTrackerModulePlayerNode::FOperatorFactory>())
    ,	Interface(DeclareVertexInterface())
{}
FConcordTrackerModulePlayerNode::FConcordTrackerModulePlayerNode(const FNodeInitData& InInitData)
    : FConcordTrackerModulePlayerNode(InInitData.InstanceName, InInitData.InstanceID)
{}

FDataReferenceCollection FConcordTrackerModulePlayerOperator::GetInputs() const
{
    FDataReferenceCollection InputDataReferences;
    InputDataReferences.AddDataReadReference("Tracker Module", TrackerModuleAsset);
    InputDataReferences.AddDataReadReference("Pattern", PatternAsset);
    InputDataReferences.AddDataReadReference("Start", Start);
    InputDataReferences.AddDataReadReference("Stop", Stop);
    InputDataReferences.AddDataReadReference("Start Line", StartLine);
    InputDataReferences.AddDataReadReference("Loop", bLoop);
    return InputDataReferences;
}

FDataReferenceCollection FConcordTrackerModulePlayerOperator::GetOutputs() const
{
    FDataReferenceCollection OutputDataReferences;
    OutputDataReferences.AddDataReadReference("Out Left", LeftAudioOutput);
    OutputDataReferences.AddDataReadReference("Out Right", RightAudioOutput);
    return OutputDataReferences;
}

void FConcordTrackerModulePlayerOperator::Execute()
{
    if (!ReinitXmp()) return;
    if (!bCleared && PatternAsset->GetProxy()->Guid != CurrentPatternGuid)
        UpdatePattern();

    if (Stop->IsTriggeredInBlock())
    {
        ClearPattern();
        bCleared = true;
    }
    else if (Start->IsTriggeredInBlock())
    {
        Start->ExecuteBlock([&](int32 BeginFrame, int32 EndFrame)
        {
            PlayModule(BeginFrame, EndFrame);
        },
        [&](int32 BeginFrame, int32 EndFrame)
        {
            if (bCleared) UpdatePattern();
            bCleared = false;
            SetPlayerStartPosition();
            PlayModule(BeginFrame, EndFrame);
        });
        return;
    }

    PlayModule(0, Settings.GetNumFramesPerBlock());
}

bool FConcordTrackerModulePlayerOperator::ReinitXmp()
{
    if (!TrackerModuleAsset->IsInitialized() || !PatternAsset->IsInitialized())
        return false;

    if (!context)
    {
        context = xmp_create_context();
        return LoadTrackerModule();
    }
    else if (TrackerModuleAsset->GetProxy()->Guid != CurrentTrackerModuleGuid)
    {
        xmp_end_player(context);
        xmp_release_module(context);
        return LoadTrackerModule();
    }
    return true;
}

bool FConcordTrackerModulePlayerOperator::LoadTrackerModule()
{
    const FConcordTrackerModuleProxy* ModuleProxy = TrackerModuleAsset->GetProxy();
    if (int error_code = xmp_load_module_from_memory(context, ModuleProxy->ModuleDataPtr->GetData(), ModuleProxy->ModuleDataPtr->Num()))
    {
        UE_LOG(LogMetaSound, Error, TEXT("xmp_load_module_from_memory failed: %i"), -error_code);
        return false;
    }
    xmp_get_module_info(context, &module_info);
    if (int error_code = xmp_start_player(context, Settings.GetSampleRate(), 0))
    {
        UE_LOG(LogMetaSound, Error, TEXT("xmp_start_player failed: %i"), -error_code);
        return false;
    }
    xmp_set_player(context, XMP_PLAYER_MIX, 100);
    CurrentTrackerModuleGuid = ModuleProxy->Guid;
    if (bCleared) ClearPattern();
    return true;
}

void FConcordTrackerModulePlayerOperator::FreeXmp()
{
    if (!ReinitXmp()) return;
    xmp_end_player(context);
    xmp_release_module(context);
    xmp_end_smix(context);
    xmp_free_context(context);
}

void FConcordTrackerModulePlayerOperator::SetPlayerStartPosition()
{
    if (int error_code = xmp_start_player(context, Settings.GetSampleRate(), 0))
    {
        UE_LOG(LogMetaSound, Error, TEXT("xmp_start_player failed: %i"), -error_code);
        return;
    }
    xmp_set_player(context, XMP_PLAYER_MIX, 100);
    const float RowDuration = (2.5f / module_info.mod->bpm) * module_info.mod->spd; // https://wiki.openmpt.org/Manual:_Song_Properties#Tempo_Mode
    int32 FramesToSkip = *StartLine * RowDuration * Settings.GetSampleRate();
    while (FramesToSkip > 0)
    {
        int32 FramesSkipped = FMath::Min(FramesToSkip, Settings.GetNumFramesPerBlock());
        xmp_play_buffer(context, XMPBuffer.GetData(), FramesSkipped * 2 * sizeof(int16), 0);
        FramesToSkip -= FramesSkipped;
    }
}

void FConcordTrackerModulePlayerOperator::PlayModule(int32 StartFrame, int32 EndFrame)
{
    const int32 NumFrames = EndFrame - StartFrame;
    xmp_play_buffer(context, XMPBuffer.GetData(), NumFrames * 2 * sizeof(int16), 0);
    for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
    {
        LeftAudioOutput->GetData()[StartFrame + FrameIndex]  = XMPBuffer[FrameIndex * 2 + 0] / float(0x7FFF);
        RightAudioOutput->GetData()[StartFrame + FrameIndex] = XMPBuffer[FrameIndex * 2 + 1] / float(0x7FFF);
    }
}

void FConcordTrackerModulePlayerOperator::UpdatePattern()
{
    ClearPattern();
    xmp_module* mod = module_info.mod;
    int32 track_index = 0;
    for (int32 instrument_index = 0; instrument_index < mod->ins; ++instrument_index)
    {
        TCHAR WideName[26];
        for (int32 Index = 0; Index < 26; ++Index)
            WideName[Index] = mod->xxi[instrument_index].name[Index];
        FStringView NameView = MakeStringView(WideName);
        const FConcordTrack* Track = PatternAsset->GetTracks().FindByHash(GetTypeHash(NameView), NameView);
        bool bIsRightChannel = false;
        if (!Track && (NameView[0] == 'M' || NameView[0] == 'L' || NameView[0] == 'R') && NameView[1] == '_')
        {
            NameView.RemovePrefix(2);
            Track = PatternAsset->GetTracks().FindByHash(GetTypeHash(NameView), NameView);
            bIsRightChannel = (WideName[0] == 'R');
        }
        if (!Track) continue;
        for (const FConcordColumn& Column : Track->Columns)
        {
            if (track_index >= mod->trk) return;
            xmp_track* track = mod->xxt[track_index++];
            for (int32 row = 0; row < track->rows; ++row)
            {
                int note = (Column.NoteValues.Num() > row) ? Column.NoteValues[row] : 0;
                if (note > 0) note = FMath::Min(note + 1, 128);
                else if (note < 0) note = XMP_KEY_OFF;

                int instrument = (Column.InstrumentValues.Num() > row) ? Column.InstrumentValues[row] : 0;
                if (instrument == 0 && note > 0) instrument = instrument_index + 1;
                else if (instrument != 0 && bIsRightChannel) ++instrument;
                if (note == XMP_KEY_OFF) instrument = 0;
                instrument = FMath::Clamp(instrument, 0, mod->ins);

                int vol = (Column.VolumeValues.Num() > row) ? Column.VolumeValues[row] : 0;
                vol = FMath::Clamp(vol, 0, 65);

                int delay = (Column.DelayValues.Num() > row) ? Column.DelayValues[row] : 0;
                delay = FMath::Clamp(delay, 0, 0x0F);

                xmp_event& event = track->event[row];
                event.note = note;
                event.ins = instrument;
                event.vol = vol;
                event.fxt = 0x0E;
                event.fxp = 0xD0 | delay;
            }
        }
    }
    CurrentPatternGuid = PatternAsset->GetProxy()->Guid;
}

void FConcordTrackerModulePlayerOperator::ClearPattern()
{
    xmp_module* mod = module_info.mod;
    for (int32 track_index = 0; track_index < mod->trk; ++track_index)
        for (int32 row = 0; row < mod->xxt[track_index]->rows; ++row)
        {
            xmp_event& event = mod->xxt[track_index]->event[row];
            event.note = XMP_KEY_OFF;
            event.fxt = 0; event.fxp = 0;
            event.f2t = 0; event.f2p = 0;
        }
}
