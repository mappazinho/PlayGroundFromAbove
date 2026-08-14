/*************************************************************************************************
*
* File: Config.cpp
*
* Description: Implements the configuration objects
*
* Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#include <Windows.h>
#include <Shlobj.h>
#include <TChar.h>

#include <fstream>
using namespace std;

#include "Config.h"
#include "Misc.h"
// Main Config class

Config &Config::GetConfig()
{
    static Config instance;
    return instance;
}

Config::Config()
{
    LoadDefaultValues();
    LoadConfigValues();
}

string Config::GetFolder()
{
    char sAppData[MAX_PATH];
    if ( FAILED( SHGetFolderPathA( NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, sAppData ) ) )
        return string();

    strcat_s( sAppData, "\\" );
    strcat_s( sAppData, APPNAME );
    if ( GetFileAttributesA( sAppData ) == INVALID_FILE_ATTRIBUTES )
        if ( !CreateDirectoryA( sAppData, NULL ) )
            return string();

    return sAppData;
}

void Config::LoadDefaultValues()
{
    m_VisualSettings.LoadDefaultValues();
    m_AudioSettings.LoadDefaultValues();
    m_VideoSettings.LoadDefaultValues();
    m_ControlsSettings.LoadDefaultValues();
    m_PlaybackSettings.LoadDefaultValues();
    m_ViewSettings.LoadDefaultValues();
    m_VizSettings.LoadDefaultValues();
}

void Config::LoadConfigValues()
{
    // Where to load?
    string sPath = GetFolder();
    if ( sPath.length() == 0 ) return;

    // Load it
    TiXmlDocument doc( sPath + "\\Config.xml" );
    if ( !doc.LoadFile() ) return;

    // Get the root element
    TiXmlElement *txRoot = doc.FirstChildElement();
    if ( !txRoot ) return;

    LoadConfigValues( txRoot );

    // Custom settings need to be loaded from a separate file, otherwise stock PFA will reset them
    doc = TiXmlDocument(sPath + "\\PlayGroundFromAbove.xml");
    if (!doc.LoadFile())
        return;

    txRoot = doc.FirstChildElement();
    if (!txRoot)
        return;

    m_VizSettings.LoadConfigValues(txRoot);
}

void Config::LoadConfigValues( TiXmlElement *txRoot )
{
    m_VisualSettings.LoadConfigValues( txRoot );
    m_AudioSettings.LoadConfigValues( txRoot );
    m_VideoSettings.LoadConfigValues( txRoot );
    m_ControlsSettings.LoadConfigValues( txRoot );
    m_SongLibrary.LoadConfigValues( txRoot );
    m_PlaybackSettings.LoadConfigValues( txRoot );
    m_ViewSettings.LoadConfigValues( txRoot );
}

bool Config::SaveConfigValues()
{
    // Where to save?
    string sPath = GetFolder();
    if ( sPath.length() == 0 ) return false;

    // Create the XML document
    TiXmlDocument doc;
    TiXmlDeclaration *decl = new TiXmlDeclaration( "1.0", "", "" );
    doc.LinkEndChild( decl );
    TiXmlElement *txRoot = new TiXmlElement( APPNAMENOSPACES );
    doc.LinkEndChild( txRoot );

    // Save each of the config
    SaveConfigValues( txRoot );

    // Write it!
    bool bStockRet = doc.SaveFile( sPath + "\\Config.xml" );

    // Same as in LoadConfigValues
    doc = TiXmlDocument();
    decl = new TiXmlDeclaration("1.0", "", "");
    doc.LinkEndChild(decl);
    txRoot = new TiXmlElement(APPNAMENOSPACES);
    doc.LinkEndChild(txRoot);

    m_VizSettings.SaveConfigValues(txRoot);

    return bStockRet && doc.SaveFile(sPath + "\\PlayGroundFromAbove.xml");
}

bool Config::SaveConfigValues( TiXmlElement *txRoot )
{
    bool bSaved = true;
    bSaved &= m_VisualSettings.SaveConfigValues( txRoot );
    bSaved &= m_AudioSettings.SaveConfigValues( txRoot );
    bSaved &= m_VideoSettings.SaveConfigValues( txRoot );
    bSaved &= m_ControlsSettings.SaveConfigValues( txRoot );
    bSaved &= m_SongLibrary.SaveConfigValues( txRoot );
    bSaved &= m_PlaybackSettings.SaveConfigValues( txRoot );
    bSaved &= m_ViewSettings.SaveConfigValues( txRoot );
    return bSaved;
}

// LoadDefaultValues

void VisualSettings::LoadDefaultValues()
{
    this->eKeysShown = All;
    this->eTransitionSpeed = SmoothSlow;
    this->bAlwaysShowControls = false;
    this->bAssociateFiles = false;
    this->iFirstKey = 0;
    this->iLastKey = 127;

    iBkgColor = 0x00303030;
    int R, G, B = 0, S = 80, V = 100;
    int iColors = sizeof( this->colors ) / sizeof( this->colors[0] );
    for ( int i = 10, count = 0; count < iColors; i = ( i + 7 ) % iColors, count++ )
    {
        Util::HSVtoRGB( 360 * i / iColors, S, V, R, G, B );
        this->colors[count] = RGB( R, G, B );
    }
    swap( this->colors[2], this->colors[4] );
}

void AudioSettings::LoadDefaultValues()
{
    this->iOutDevice = -1;
    this->bPreRenderAudio = false;
    this->sPreSoundfontPath = L"";
    this->sPreSoundfontDir = L"";
    this->iPreVoices = 4096;
    this->dPreFPS = 0.0;
    this->iPreLMAttack = 10;
    this->iPreLMRelease = 1000;
    this->bNoFX = false;
    this->iPreVelThreshLow = 0;
    this->iPreVelThreshUpp = 127;
    this->bPreUnderrunRepeat = false;
    this->bPreRepeatCustom = false;
    this->iPreRepeatMs = 250;
    this->bPreStutterOnLag = true;
    this->iPreBufferMs = 30000;
    LoadMIDIDevices();
}

void VideoSettings::LoadDefaultValues()
{
    this->bLimitFPS = true;
    this->bShowFPS = false;
    this->eRenderer = DirectX12;
}

void ControlsSettings::LoadDefaultValues()
{
    this->dFwdBackSecs = 3.0;
    this->dSpeedUpPct = 10.0;
}

void PlaybackSettings::LoadDefaultValues()
{
    this->m_ePlayMode = GameState::Splash;
    this->m_bMute = false;
    this->m_bPlayable = false;
    this->m_bPaused = true;
    this->m_dSpeed = 1.0;
    this->m_dNSpeed = 1.0;
    this->m_dVolume = 1.0;
}

void ViewSettings::LoadDefaultValues()
{
    this->m_bLibrary = true;
    this->m_bControls = true;
    this->m_bKeyboard = true;
    this->m_bOnTop = false;
    this->m_bFullScreen = false;
    this->m_fOffsetX = 0.0f;
    this->m_fOffsetY = 0.0f;
    this->m_fZoomX = 1.0f;
    this->m_iMainLeft = CW_USEDEFAULT;
    this->m_iMainTop = CW_USEDEFAULT;
    this->m_iMainWidth = 960;
    this->m_iMainHeight = 589;
    this->m_iLibWidth = 0;
}

void VizSettings::LoadDefaultValues() {
    this->bTickBased = false;
    this->bShowMarkers = true;
    this->eMarkerEncoding = MarkerEncoding::CP1252;
    this->bNerdStats = false;
    this->sSplashMIDI = L"";
    this->bVisualizePitchBends = false;
    this->bDualPianoRoll = false;
    this->bDualRollKeyboard = true;
    this->bBloom = false;
    this->fBloomIntensity = 1.0f;
    this->fBloomBrightness = 1.0f;
    this->fBloomSpread = 10.0f;
    this->fRibbonBloomHeight = 60.0f;
    this->fRibbonBloomIntensity = 1.0f;
    this->fRibbonBloomBrightness = 1.0f;
    this->iRibbonBloomSteps = 64;
    this->fBloomSaturation = 1.2f;
    this->bColoredRibbon = true;
    this->bDumpFrames = false;
    this->iBarColor = 0x00FF0080;
    this->sBackground = L"";
    this->fBGBlur = 0.0f;
    this->fBGOpacity = 1.0f;
    this->bColorLoop = false;
    this->bKDMAPI = false;
    this->bDisableUI = false;
    this->fUIScale = 1.0f;
    this->sUIFont = L"";
}

void AudioSettings::LoadMIDIDevices()
{
    wstring oldOutDev( this->iOutDevice >= 0 ? this->vMIDIOutDevices[this->iOutDevice] : L"" );
    this->iOutDevice = -1;
    this->vMIDIOutDevices.clear();
    int iNumOutDevs = midiOutGetNumDevs();
    for ( int i = 0; i < iNumOutDevs; i++ )
    {
        MIDIOUTCAPS moc;
        midiOutGetDevCaps( i, &moc, sizeof( MIDIOUTCAPS ) );
        this->vMIDIOutDevices.push_back( moc.szPname );

        if ( this->sDesiredOut == this->vMIDIOutDevices[i] )
            this->iOutDevice = i;
        if ( oldOutDev == this->vMIDIOutDevices[i] && this->iOutDevice < 0 )
            this->iOutDevice = i;
    }
    if ( this->iOutDevice < 0 )
        this->iOutDevice = iNumOutDevs - 1;
}

// LoadConfigValues

void VisualSettings::LoadConfigValues( TiXmlElement *txRoot )
{
    TiXmlElement *txVisual = txRoot->FirstChildElement( "Visual" );
    if ( !txVisual ) return;

    // Attributes
    int iAttrVal;
    if ( txVisual->QueryIntAttribute( "KeysShown", &iAttrVal ) == TIXML_SUCCESS )
        this->eKeysShown = static_cast< KeysShown >( iAttrVal );
    if ( this->eKeysShown == Custom )
        this->eKeysShown = All;
    if ( txVisual->QueryIntAttribute( "TransitionSpeed", &iAttrVal ) == TIXML_SUCCESS )
        this->eTransitionSpeed = static_cast< TransitionSpeed >( iAttrVal );
    if ( txVisual->QueryIntAttribute( "AlwaysShowControls", &iAttrVal ) == TIXML_SUCCESS )
        this->bAlwaysShowControls = ( iAttrVal != 0 );
    if ( txVisual->QueryIntAttribute( "AssociateFiles", &iAttrVal ) == TIXML_SUCCESS )
        this->bAssociateFiles = ( iAttrVal != 0 );

    // Colors
    int r, g, b = 0;
    size_t i = 0;
    TiXmlElement *txColors = txVisual->FirstChildElement( "Colors" );
    if ( txColors )
        for ( TiXmlElement *txColor = txColors->FirstChildElement( "Color" );
              txColor && i < sizeof( this->colors ) / sizeof( this->colors[0] );
              txColor = txColor->NextSiblingElement( "Color" ), i++ )
            if ( txColor->QueryIntAttribute( "R", &r ) == TIXML_SUCCESS &&
                 txColor->QueryIntAttribute( "G", &g ) == TIXML_SUCCESS &&
                 txColor->QueryIntAttribute( "B", &b ) == TIXML_SUCCESS )
                this->colors[i] = ( ( r & 0xFF ) << 0 ) | ( ( g & 0xFF ) << 8 ) | ( ( b & 0xFF ) << 16 );
    
    TiXmlElement *txBkgColor = txVisual->FirstChildElement( "BkgColor" );
    if ( txBkgColor )
        if ( txBkgColor->QueryIntAttribute( "R", &r ) == TIXML_SUCCESS &&
             txBkgColor->QueryIntAttribute( "G", &g ) == TIXML_SUCCESS &&
             txBkgColor->QueryIntAttribute( "B", &b ) == TIXML_SUCCESS )
            this->iBkgColor = ( ( r & 0xFF ) << 0 ) | ( ( g & 0xFF ) << 8 ) | ( ( b & 0xFF ) << 16 );
}

void AudioSettings::LoadConfigValues( TiXmlElement *txRoot )
{
    TiXmlElement *txAudio = txRoot->FirstChildElement( "Audio" );
    if ( !txAudio ) return;

    string sMIDIOutDevice;
    if ( txAudio->QueryStringAttribute( "MIDIOutDevice", &sMIDIOutDevice ) == TIXML_SUCCESS )
    {
        this->sDesiredOut = Util::StringToWstring( sMIDIOutDevice );
        for ( size_t i = 0; i < this->vMIDIOutDevices.size(); i++ )
            if ( this->vMIDIOutDevices[i] == this->sDesiredOut )
                this->iOutDevice = (int)i;
    }

    int iAttrVal = 0;
    if ( txAudio->QueryIntAttribute( "PreRenderAudio", &iAttrVal ) == TIXML_SUCCESS )
        this->bPreRenderAudio = (iAttrVal != 0);

    string sPreSoundfontPath;
    if ( txAudio->QueryStringAttribute( "PreSoundfontPath", &sPreSoundfontPath ) == TIXML_SUCCESS )
        this->sPreSoundfontPath = Util::StringToWstring( sPreSoundfontPath );

    string sPreSoundfontDir;
    if ( txAudio->QueryStringAttribute( "PreSoundfontDir", &sPreSoundfontDir ) == TIXML_SUCCESS )
        this->sPreSoundfontDir = Util::StringToWstring( sPreSoundfontDir );

    txAudio->QueryIntAttribute( "PreAudVoices", &iPreVoices );
    txAudio->QueryDoubleAttribute( "PreAudFPS", &dPreFPS );
    txAudio->QueryIntAttribute( "PreAudLimiterAttack", &iPreLMAttack );
    txAudio->QueryIntAttribute( "PreAudLimiterRelease", &iPreLMRelease );
    int iAttrVal2 = 0;
    if ( txAudio->QueryIntAttribute( "PreAudNoFX", &iAttrVal2 ) == TIXML_SUCCESS )
        this->bNoFX = (iAttrVal2 != 0);
    txAudio->QueryIntAttribute( "PreAudVelThreshLow", &iPreVelThreshLow );
    txAudio->QueryIntAttribute( "PReAudVelThreshUpp", &iPreVelThreshUpp );
    int iAttrVal3 = 0;
    if ( txAudio->QueryIntAttribute( "PreAudUnderrunRepeat", &iAttrVal3 ) == TIXML_SUCCESS )
        this->bPreUnderrunRepeat = (iAttrVal3 != 0);
    int iAttrVal4 = 0;
    if ( txAudio->QueryIntAttribute( "PreAudRepeatCustom", &iAttrVal4 ) == TIXML_SUCCESS )
        this->bPreRepeatCustom = (iAttrVal4 != 0);
    txAudio->QueryIntAttribute( "PreAudRepeatMs", &iPreRepeatMs );
    int iAttrVal5 = 0;
    if ( txAudio->QueryIntAttribute( "PreAudStutterOnLag", &iAttrVal5 ) == TIXML_SUCCESS )
        this->bPreStutterOnLag = (iAttrVal5 != 0);
    this->iPreBufferMs = 30000;
    if ( txAudio->QueryIntAttribute( "PreAudBufferMs", &iPreBufferMs ) != TIXML_SUCCESS || iPreBufferMs < 1000 )
        this->iPreBufferMs = 30000;
}

void VideoSettings::LoadConfigValues( TiXmlElement *txRoot )
{
    TiXmlElement *txVideo = txRoot->FirstChildElement( "Video" );
    if ( !txVideo ) return;

    int iAttrVal;
    if ( txVideo->QueryIntAttribute( "ShowFPS", &iAttrVal ) == TIXML_SUCCESS )
        this->bShowFPS = ( iAttrVal != 0 );
    if ( txVideo->QueryIntAttribute( "LimitFPS", &iAttrVal ) == TIXML_SUCCESS )
        this->bLimitFPS = ( iAttrVal != 0 );
    // New renderer attribute (0 = DirectX 11, 1 = DirectX 12). The legacy
    // "Renderer" attribute (Direct3D/OpenGL/GDI) no longer exists; old configs
    // migrate to DirectX 12, which was the only working backend.
    if ( txVideo->QueryIntAttribute( "RendererAPI", &iAttrVal ) == TIXML_SUCCESS && ( iAttrVal == 0 || iAttrVal == 1 ) )
        this->eRenderer = static_cast< Renderer >( iAttrVal );
}

void ControlsSettings::LoadConfigValues( TiXmlElement *txRoot )
{
    TiXmlElement *txControls = txRoot->FirstChildElement( "Controls" );
    if ( !txControls ) return;

    txControls->QueryDoubleAttribute( "FwdBackSecs", &this->dFwdBackSecs );
    txControls->QueryDoubleAttribute( "SpeedUpPct", &this->dSpeedUpPct );
}

void SongLibrary::LoadConfigValues(TiXmlElement* txRoot)
{
    TiXmlElement* txLibrary = txRoot->FirstChildElement("Library");
    if (!txLibrary) return;

    int iAttrVal;
    if (txLibrary->QueryIntAttribute("AlwaysAdd", &iAttrVal) == TIXML_SUCCESS)
        m_bAlwaysAdd = (iAttrVal != 0);
    txLibrary->QueryIntAttribute("SortCol", &m_iSortCol);

    TiXmlElement* txSources = txLibrary->FirstChildElement("Sources");
    if (txSources)
    {
        m_mSources.clear();
        int iSourceType;
        string sSource;
        for (TiXmlElement* txSource = txSources->FirstChildElement("Source"); txSource;
            txSource = txSource->NextSiblingElement("Source"))
            if (txSource->QueryStringAttribute("Name", &sSource) == TIXML_SUCCESS &&
                txSource->QueryIntAttribute("Type", &iSourceType) == TIXML_SUCCESS)
                AddSource(Util::StringToWstring(sSource), static_cast<Source>(iSourceType), false);
    }
}

void PlaybackSettings::LoadConfigValues( TiXmlElement *txRoot )
{
    TiXmlElement *txPlayback = txRoot->FirstChildElement( "Playback" );
    if ( !txPlayback ) return;

    int iAttrVal;
    if ( txPlayback->QueryIntAttribute( "Mute", &iAttrVal ) == TIXML_SUCCESS )
        m_bMute = ( iAttrVal != 0 );
    txPlayback->QueryDoubleAttribute( "NoteSpeed", &m_dNSpeed);
    if (m_dNSpeed < 0.00001)
        m_dNSpeed = 1.0;
    txPlayback->QueryDoubleAttribute( "Volume", &m_dVolume );
}

void ViewSettings::LoadConfigValues( TiXmlElement *txRoot )
{
    TiXmlElement *txView = txRoot->FirstChildElement( "View" );
    if ( !txView ) return;

    int iAttrVal;
    if (txView->QueryIntAttribute("Library", &iAttrVal) == TIXML_SUCCESS)
        m_bLibrary = (iAttrVal != 0);
    if ( txView->QueryIntAttribute( "Controls", &iAttrVal ) == TIXML_SUCCESS )
        m_bControls = ( iAttrVal != 0 );
    if ( txView->QueryIntAttribute( "Keyboard", &iAttrVal ) == TIXML_SUCCESS )
        m_bKeyboard = ( iAttrVal != 0 );
    if ( txView->QueryIntAttribute( "OnTop", &iAttrVal ) == TIXML_SUCCESS )
        m_bOnTop = ( iAttrVal != 0 );
    txView->QueryFloatAttribute( "OffsetX", &m_fOffsetX );
    txView->QueryFloatAttribute( "OffsetY", &m_fOffsetY );
    txView->QueryFloatAttribute( "ZoomX", &m_fZoomX );
    txView->QueryIntAttribute( "MainLeft", &m_iMainLeft );
    txView->QueryIntAttribute( "MainTop", &m_iMainTop );
    txView->QueryIntAttribute( "MainWidth", &m_iMainWidth );
    txView->QueryIntAttribute( "MainHeight", &m_iMainHeight );
    txView->QueryIntAttribute( "LibWidth", &m_iLibWidth );
}

void VizSettings::LoadConfigValues(TiXmlElement* txRoot) {
    TiXmlElement* txViz = txRoot->FirstChildElement("Viz");
    if (!txViz)
        return;

    int iAttrVal;
    if (txViz->QueryIntAttribute("TickBased", &iAttrVal) == TIXML_SUCCESS)
        bTickBased = (iAttrVal != 0);
    if (txViz->QueryIntAttribute("ShowMarkers", &iAttrVal) == TIXML_SUCCESS)
        bShowMarkers = (iAttrVal != 0);
    if (txViz->QueryIntAttribute("NerdStats", &iAttrVal) == TIXML_SUCCESS)
        bNerdStats = (iAttrVal != 0);
    if (txViz->QueryIntAttribute("VisualizePitchBends", &iAttrVal) == TIXML_SUCCESS)
        bVisualizePitchBends = (iAttrVal != 0);
    if (txViz->QueryIntAttribute("DualPianoRoll", &iAttrVal) == TIXML_SUCCESS)
        bDualPianoRoll = (iAttrVal != 0);
    if (txViz->QueryIntAttribute("DualRollKeyboard", &iAttrVal) == TIXML_SUCCESS)
        bDualRollKeyboard = (iAttrVal != 0);
    if (txViz->QueryIntAttribute("Bloom", &iAttrVal) == TIXML_SUCCESS)
        bBloom = (iAttrVal != 0);
    txViz->QueryFloatAttribute("BloomIntensity", &fBloomIntensity);
    fBloomIntensity = max(0.0f, min(fBloomIntensity, 1.0f));
    txViz->QueryFloatAttribute("BloomBrightness", &fBloomBrightness);
    fBloomBrightness = max(0.0f, min(fBloomBrightness, 4.0f));
    txViz->QueryFloatAttribute("BloomSpread", &fBloomSpread);
    fBloomSpread = max(1.0f, min(fBloomSpread, 30.0f));
    txViz->QueryFloatAttribute("RibbonBloomHeight", &fRibbonBloomHeight);
    fRibbonBloomHeight = max(0.0f, min(fRibbonBloomHeight, 300.0f));
    txViz->QueryFloatAttribute("RibbonBloomIntensity", &fRibbonBloomIntensity);
    fRibbonBloomIntensity = max(0.0f, min(fRibbonBloomIntensity, 4.0f));
    txViz->QueryFloatAttribute("RibbonBloomBrightness", &fRibbonBloomBrightness);
    fRibbonBloomBrightness = max(0.0f, min(fRibbonBloomBrightness, 4.0f));
    txViz->QueryIntAttribute("RibbonBloomSteps", &iRibbonBloomSteps);
    iRibbonBloomSteps = max(1, min(iRibbonBloomSteps, 100));
    txViz->QueryFloatAttribute("BloomSaturation", &fBloomSaturation);
    fBloomSaturation = max(0.0f, min(fBloomSaturation, 3.0f));
    if (txViz->QueryIntAttribute("ColoredRibbon", &iAttrVal) == TIXML_SUCCESS)
        bColoredRibbon = (iAttrVal != 0);
    if (txViz->QueryIntAttribute("DumpFrames", &iAttrVal) == TIXML_SUCCESS)
        bDumpFrames = (iAttrVal != 0);
    if (txViz->QueryIntAttribute("ColorLoop", &iAttrVal) == TIXML_SUCCESS)
        bColorLoop = (iAttrVal != 0);
    if (txViz->QueryIntAttribute("KDMAPI", &iAttrVal) == TIXML_SUCCESS)
        bKDMAPI = (iAttrVal != 0);
    if (txViz->QueryIntAttribute("DisableUI", &iAttrVal) == TIXML_SUCCESS)
        bDisableUI = (iAttrVal != 0);
    std::string sTempStr;
    txViz->QueryStringAttribute("SplashMIDI", &sTempStr);
    sSplashMIDI = Util::StringToWstring(sTempStr);
    sTempStr = "";
    txViz->QueryStringAttribute("Background", &sTempStr);
    sBackground = Util::StringToWstring(sTempStr);
    txViz->QueryFloatAttribute("BGBlur", &fBGBlur);
    fBGBlur = max(0.0f, min(fBGBlur, 100.0f));
    if (std::isnan(fBGBlur)) fBGBlur = 0.0f;
    txViz->QueryFloatAttribute("BGOpacity", &fBGOpacity);
    fBGOpacity = max(0.0f, min(fBGOpacity, 1.0f));
    if (std::isnan(fBGOpacity)) fBGOpacity = 1.0f;
    txViz->QueryIntAttribute("MarkerEncoding", (int*)&eMarkerEncoding);
    eMarkerEncoding = max(MarkerEncoding::CP1252, min(eMarkerEncoding, MarkerEncoding::UTF8));
    txViz->QueryFloatAttribute("UIScale", &fUIScale);
    fUIScale = max(0.1f, min(fUIScale, 10.0f));
    if (std::isnan(fUIScale))
        fUIScale = 1.0f;
    txViz->QueryStringAttribute("UIFont", &sTempStr);
    sUIFont = Util::StringToWstring(sTempStr);

    int r, g, b = 0;
    TiXmlElement* txBarColor = txViz->FirstChildElement("BarColor");
    if (txBarColor)
        if (txBarColor->QueryIntAttribute("R", &r) == TIXML_SUCCESS &&
            txBarColor->QueryIntAttribute("G", &g) == TIXML_SUCCESS &&
            txBarColor->QueryIntAttribute("B", &b) == TIXML_SUCCESS)
            iBarColor = ((r & 0xFF) << 0) | ((g & 0xFF) << 8) | ((b & 0xFF) << 16);
}

// SaveConfigValues

bool VisualSettings::SaveConfigValues( TiXmlElement *txRoot )
{
    TiXmlElement *txVisual = new TiXmlElement( "Visual" );
    txRoot->LinkEndChild( txVisual );
    txVisual->SetAttribute( "KeysShown", this->eKeysShown );
    txVisual->SetAttribute( "TransitionSpeed", this->eTransitionSpeed );
    txVisual->SetAttribute( "AlwaysShowControls", this->bAlwaysShowControls );
    txVisual->SetAttribute( "AssociateFiles", this->bAssociateFiles );
    txVisual->SetAttribute( "FirstKey", this->iFirstKey );
    txVisual->SetAttribute( "LastKey", this->iLastKey );

    TiXmlElement *txColors = new TiXmlElement( "Colors" );
    txVisual->LinkEndChild( txColors );
    for ( size_t i = 0; i < sizeof( this->colors ) / sizeof( this->colors[0] ); i++ )
    {
        TiXmlElement *txColor = new TiXmlElement( "Color" );
        txColors->LinkEndChild( txColor );
        txColor->SetAttribute( "R", ( this->colors[i] >>  0 ) & 0xFF );
        txColor->SetAttribute( "G", ( this->colors[i] >>  8 ) & 0xFF );
        txColor->SetAttribute( "B", ( this->colors[i] >> 16 ) & 0xFF );
    }

    TiXmlElement *txBkgColor = new TiXmlElement( "BkgColor" );
    txVisual->LinkEndChild( txBkgColor );
    txBkgColor->SetAttribute( "R", ( this->iBkgColor >>  0 ) & 0xFF );
    txBkgColor->SetAttribute( "G", ( this->iBkgColor >>  8 ) & 0xFF );
    txBkgColor->SetAttribute( "B", ( this->iBkgColor >> 16 ) & 0xFF );

    return true;
}

bool AudioSettings::SaveConfigValues( TiXmlElement *txRoot )
{
    TiXmlElement *txAudio = new TiXmlElement( "Audio" );
    txRoot->LinkEndChild( txAudio );

    if ( this->sDesiredOut.length() > 0 )
        txAudio->SetAttribute( "MIDIOutDevice", Util::WstringToString( this->sDesiredOut ) );
    txAudio->SetAttribute( "PreRenderAudio", this->bPreRenderAudio );
    if ( this->sPreSoundfontPath.length() > 0 )
        txAudio->SetAttribute( "PreSoundfontPath", Util::WstringToString( this->sPreSoundfontPath ) );
    if ( this->sPreSoundfontDir.length() > 0 )
        txAudio->SetAttribute( "PreSoundfontDir", Util::WstringToString( this->sPreSoundfontDir ) );
    txAudio->SetAttribute( "PreAudVoices", iPreVoices );
    txAudio->SetAttribute( "PreAudFPS", dPreFPS );
    txAudio->SetAttribute( "PreAudLimiterAttack", iPreLMAttack );
    txAudio->SetAttribute( "PreAudLimiterRelease", iPreLMRelease );
    txAudio->SetAttribute( "PreAudNoFX", bNoFX );
    txAudio->SetAttribute( "PreAudVelThreshLow", iPreVelThreshLow );
    txAudio->SetAttribute( "PreAudVelThreshUpp", iPreVelThreshUpp );
    txAudio->SetAttribute( "PreAudUnderrunRepeat", bPreUnderrunRepeat );
    txAudio->SetAttribute( "PreAudRepeatCustom", bPreRepeatCustom );
    txAudio->SetAttribute( "PreAudRepeatMs", iPreRepeatMs );
    txAudio->SetAttribute( "PreAudStutterOnLag", bPreStutterOnLag );
    txAudio->SetAttribute( "PreAudBufferMs", iPreBufferMs );

    return true;
}

bool VideoSettings::SaveConfigValues( TiXmlElement *txRoot )
{
    TiXmlElement *txVideo = new TiXmlElement( "Video" );
    txRoot->LinkEndChild( txVideo );
    txVideo->SetAttribute( "Renderer", 0 ); // Legacy attribute: always "Direct3D"
    txVideo->SetAttribute( "RendererAPI", this->eRenderer );
    txVideo->SetAttribute( "ShowFPS", this->bShowFPS );
    txVideo->SetAttribute( "LimitFPS", this->bLimitFPS );
    return true;
}

bool ControlsSettings::SaveConfigValues( TiXmlElement *txRoot )
{
    TiXmlElement *txControls = new TiXmlElement( "Controls" );
    txRoot->LinkEndChild( txControls );
    txControls->SetDoubleAttribute( "FwdBackSecs", this->dFwdBackSecs );
    txControls->SetDoubleAttribute( "SpeedUpPct", this->dSpeedUpPct );
    return true;
}

bool SongLibrary::SaveConfigValues(TiXmlElement* txRoot)
{
    TiXmlElement* txLibrary = new TiXmlElement("Library");
    txRoot->LinkEndChild(txLibrary);
    txLibrary->SetAttribute("AlwaysAdd", m_bAlwaysAdd);
    txLibrary->SetAttribute("SortCol", m_iSortCol);

    TiXmlElement* txSources = new TiXmlElement("Sources");
    txLibrary->LinkEndChild(txSources);
    for (map< wstring, Source >::const_iterator it = m_mSources.begin(); it != m_mSources.end(); ++it)
    {
        TiXmlElement* txSource = new TiXmlElement("Source");
        txSources->LinkEndChild(txSource);
        txSource->SetAttribute("Name", Util::WstringToString(it->first));
        txSource->SetAttribute("Type", it->second);
    }
    return true;
}

bool PlaybackSettings::SaveConfigValues( TiXmlElement *txRoot )
{
    TiXmlElement *txPlayback = new TiXmlElement( "Playback" );
    txRoot->LinkEndChild( txPlayback );
    txPlayback->SetAttribute( "Mute", m_bMute );
    txPlayback->SetDoubleAttribute( "Volume", m_dVolume );
    txPlayback->SetDoubleAttribute( "NoteSpeed", m_dNSpeed );
    return true;
}

bool ViewSettings::SaveConfigValues( TiXmlElement *txRoot )
{
    TiXmlElement *txView = new TiXmlElement( "View" );
    txRoot->LinkEndChild( txView );
    txView->SetAttribute("Library", m_bLibrary);
    txView->SetAttribute( "Controls", m_bControls );
    txView->SetAttribute( "Keyboard", m_bKeyboard );
    txView->SetAttribute( "OnTop", m_bOnTop );
    txView->SetDoubleAttribute( "OffsetX", m_fOffsetX );
    txView->SetDoubleAttribute( "OffsetY", m_fOffsetY );
    txView->SetDoubleAttribute( "ZoomX", m_fZoomX );
    txView->SetAttribute( "MainLeft", m_iMainLeft );
    txView->SetAttribute( "MainTop", m_iMainTop );
    txView->SetAttribute( "MainWidth", m_iMainWidth );
    txView->SetAttribute( "MainHeight", m_iMainHeight );
    txView->SetAttribute( "LibWidth", m_iLibWidth );
    return true;
}

bool VizSettings::SaveConfigValues(TiXmlElement* txRoot) {
    TiXmlElement* txViz = new TiXmlElement("Viz");
    txRoot->LinkEndChild(txViz);
    txViz->SetAttribute("TickBased", bTickBased);
    txViz->SetAttribute("ShowMarkers", bShowMarkers);
    txViz->SetAttribute("MarkerEncoding", eMarkerEncoding);
    txViz->SetAttribute("NerdStats", bNerdStats);
    txViz->SetAttribute("SplashMIDI", Util::WstringToString(sSplashMIDI));
    txViz->SetAttribute("VisualizePitchBends", bVisualizePitchBends);
    txViz->SetAttribute("DualPianoRoll", bDualPianoRoll);
    txViz->SetAttribute("DualRollKeyboard", bDualRollKeyboard);
    txViz->SetAttribute("Bloom", bBloom);
    txViz->SetDoubleAttribute("BloomIntensity", fBloomIntensity);
    txViz->SetDoubleAttribute("BloomBrightness", fBloomBrightness);
    txViz->SetDoubleAttribute("BloomSpread", fBloomSpread);
    txViz->SetDoubleAttribute("RibbonBloomHeight", fRibbonBloomHeight);
    txViz->SetDoubleAttribute("RibbonBloomIntensity", fRibbonBloomIntensity);
    txViz->SetDoubleAttribute("RibbonBloomBrightness", fRibbonBloomBrightness);
    txViz->SetAttribute("RibbonBloomSteps", iRibbonBloomSteps);
    txViz->SetDoubleAttribute("BloomSaturation", fBloomSaturation);
    txViz->SetAttribute("ColoredRibbon", bColoredRibbon);
    txViz->SetAttribute("DumpFrames", bDumpFrames);
    txViz->SetAttribute("Background", Util::WstringToString(sBackground));
    txViz->SetDoubleAttribute("BGBlur", fBGBlur);
    txViz->SetDoubleAttribute("BGOpacity", fBGOpacity);
    txViz->SetAttribute("ColorLoop", bColorLoop);
    txViz->SetAttribute("KDMAPI", bKDMAPI);
    txViz->SetAttribute("DisableUI", bDisableUI);
    txViz->SetDoubleAttribute("UIScale", fUIScale);
    txViz->SetAttribute("UIFont", Util::WstringToString(sUIFont));

    TiXmlElement* txBarColor = new TiXmlElement("BarColor");
    txViz->LinkEndChild(txBarColor);
    txBarColor->SetAttribute("R", (iBarColor >> 0) & 0xFF);
    txBarColor->SetAttribute("G", (iBarColor >> 8) & 0xFF);
    txBarColor->SetAttribute("B", (iBarColor >> 16) & 0xFF);
    return true;
}

// Library stubs

int SongLibrary::AddSource(const wstring& sSource, Source eSource, bool)
{
    m_mSources[sSource] = eSource;
    return 1;
}
