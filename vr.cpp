// Hyperbolic Rogue -- VR support
// Copyright (C) 2020-2020 Zeno Rogue, see 'hyper.cpp' for details

/** \file vr.cpp
 *  \brief VR support
 */

#include "hyper.h"
namespace hr {

EX namespace vrhr {

#if CAP_VR

#if HDR
enum class eHeadset { none, rotation_only, reference, holonomy };
enum class eEyes { none, equidistant, truesim };
enum class eCompScreen { none, reference, single, eyes };
#endif

EX eHeadset hsm = eHeadset::reference;
EX eEyes eyes = eEyes::equidistant;
EX eCompScreen cscr = eCompScreen::single;

EX cell *forward_cell;

EX ld vraim_x, vraim_y, vrgo_x, vrgo_y;

vector<pair<string, string> > headset_desc = {
  {"none", "Ignore the headset movement and rotation."},
  {"rotation only", "Ignore the headset movement but do not ignore its rotation."},
  {"reference", "The reference point in the real world corresponds to the reference point in VR. When you move your head in a loop, you return to where you started."},
  {"holonomy", "Headsets movements in the real world are translated to the same movements in VR. Since the geometry is different, when you move your head in a loop, you usually don't return "
   "to where you started."}
  };

vector<pair<string, string> > eyes_desc = {
  {"none", "Both eyes see the same image."},
  {"equidistant", "Render the image so that the perceived direction and distance is correct."},
  {"true vision", "Simulate the actual binocular vision in the non-Euclidean space. Hyperbolic spaces look smaller than they are (stretched Klein model), spherical spaces look weird, "
    "nonisotropic spaces are incomprehensible."}, /* not implemented */
  };

/* not implemented */
vector<pair<string, string> > comp_desc = {
  {"none", "Do not display anything on the computer screen."},
  {"reference", "Display the view from the reference point."},
  {"single", "(not implemented)"}, // "Display a single monocular image."},
  {"eyes", "Display a copy of the VR display."},
  };

struct vr_rendermodel {
  string name;
  GLuint texture_id;
  vector<glhr::textured_vertex> vertices;
  };

struct vr_framebuffer {
  bool ok;
  GLuint m_nDepthBufferId;
  GLuint m_nRenderTextureId;
  GLuint m_nRenderFramebufferId;
  GLuint m_nResolveTextureId;
  GLuint m_nResolveFramebufferId;
  vr_framebuffer(int x, int y);
  ~vr_framebuffer();
  };

vr_framebuffer::vr_framebuffer(int xsize, int ysize) {
  resetbuffer rb;
  glGenFramebuffers(1, &m_nRenderFramebufferId );
  glBindFramebuffer(GL_FRAMEBUFFER, m_nRenderFramebufferId);

  glGenRenderbuffers(1, &m_nDepthBufferId);
  glBindRenderbuffer(GL_RENDERBUFFER, m_nDepthBufferId);
  glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH24_STENCIL8, xsize, ysize );
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_nDepthBufferId );
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_nDepthBufferId );

  glGenTextures(1, &m_nRenderTextureId );
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, m_nRenderTextureId );
  glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8, xsize, ysize, true);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, m_nRenderTextureId, 0);

  glGenFramebuffers(1, &m_nResolveFramebufferId );
  glBindFramebuffer(GL_FRAMEBUFFER, m_nResolveFramebufferId);

  glGenTextures(1, &m_nResolveTextureId );
  glBindTexture(GL_TEXTURE_2D, m_nResolveTextureId );
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, xsize, ysize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_nResolveTextureId, 0);

  // check FBO status
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  
  ok = status == GL_FRAMEBUFFER_COMPLETE;
  
  rb.reset();
  }

vr_framebuffer::~vr_framebuffer() {
  glDeleteRenderbuffers( 1, &m_nDepthBufferId );
  glDeleteTextures( 1, &m_nRenderTextureId );
  glDeleteFramebuffers( 1, &m_nRenderFramebufferId );
  glDeleteTextures( 1, &m_nResolveTextureId );
  glDeleteFramebuffers( 1, &m_nResolveFramebufferId );
  }

struct controller_data {
  int x, y, clicked;
  };

struct vrdata_t {
  vr::IVRSystem *vr;
  uint32_t xsize, ysize;
  vr_framebuffer *eyes[2];
  transmatrix proj[2];
  transmatrix eyepos[2];
  vr::TrackedDevicePose_t poses[ vr::k_unMaxTrackedDeviceCount ];
  transmatrix pose_matrix[vr::k_unMaxTrackedDeviceCount ];
  vector<vr_rendermodel*> models;
  vr_rendermodel* device_models[ vr::k_unMaxTrackedDeviceCount ];
  controller_data cdata [ vr::k_unMaxTrackedDeviceCount ];
  };

vrdata_t vrdata;

/** should we try to access VR */
EX bool enabled = false;

/** we tried to access VR but failed */
EX bool failed;

/** VR error message */
EX string error_msg;

/** 0 = not loaded, 1 = loaded but not currently rendering, 2 = currently rendering the VR screen, 3 = currently rendering the computer screen */
EX int state = 0;

// use E4 when working with real-world matrices to ensure that inverses, multiplications, etc. are computed correctly
#define E4 dynamicval<eGeometry> g(geometry, gCubeTiling)

#define IN_E4(x) [&]{ E4; return x; }()

std::string GetTrackedDeviceString( vr::TrackedDeviceIndex_t unDevice, vr::TrackedDeviceProperty prop, vr::TrackedPropertyError *peError = NULL ) {
  uint32_t unRequiredBufferLen = vr::VRSystem()->GetStringTrackedDeviceProperty( unDevice, prop, NULL, 0, peError );
  if( unRequiredBufferLen == 0 ) return "";

  char *pchBuffer = new char[ unRequiredBufferLen ];
  unRequiredBufferLen = vr::VRSystem()->GetStringTrackedDeviceProperty( unDevice, prop, pchBuffer, unRequiredBufferLen, peError );
  std::string sResult = pchBuffer;
  delete [] pchBuffer;
  return sResult;
  }

transmatrix vr_to_hr(vr::HmdMatrix44_t mat) {
  transmatrix T;
  for(int i=0; i<4; i++)
  for(int j=0; j<4; j++)
    T[i][j] = mat.m[i][j];
  return T;
  }

transmatrix vr_to_hr(vr::HmdMatrix34_t mat) {
  transmatrix T;
  for(int i=0; i<3; i++)
  for(int j=0; j<4; j++)
    T[i][j] = mat.m[i][j];
  T[3][0] = 0;
  T[3][1] = 0;
  T[3][2] = 0;
  T[3][3] = 1;
  return T;
  }

string device_class_name(vr::ETrackedDeviceClass v) {
  if(v == vr::TrackedDeviceClass_Controller)
    return "controller";
  if(v == vr::TrackedDeviceClass_HMD)
    return "HMD";
  if(v == vr::TrackedDeviceClass_Invalid)
    return "invalid";
  if(v == vr::TrackedDeviceClass_GenericTracker)
    return "tracker";
  if(v == vr::TrackedDeviceClass_TrackingReference)
    return "reference";
  return "unknown";
  }

EX bool first = true;

EX transmatrix hmd_at = Id;
EX transmatrix hmd_ref_at = Id;

EX transmatrix hmd_mvp, hmd_pre;

EX transmatrix sm;

vr_rendermodel *get_render_model(string name) {
  for(auto& m: vrdata.models)
    if(m->name == name)
      return m;
    
  println(hlog, "trying to load model ", name);
  
  vr::RenderModel_t *pModel;
  vr::EVRRenderModelError error;
  while(1) {
    error = vr::VRRenderModels()->LoadRenderModel_Async(name.c_str(), &pModel );
    if(error != vr::VRRenderModelError_Loading) break;
    usleep(1000);
    }
  if(error != vr::VRRenderModelError_None) {
    println(hlog, "Unable to load render model %s - %s\n", name, vr::VRRenderModels()->GetRenderModelErrorNameFromEnum( error ) );
    return NULL;
    }

  vr::RenderModel_TextureMap_t *pTexture;
  while (1) {
    error = vr::VRRenderModels()->LoadTexture_Async( pModel->diffuseTextureId, &pTexture );
    if(error != vr::VRRenderModelError_Loading) break;
    usleep(1000);
    }
  if(error != vr::VRRenderModelError_None) {
    println(hlog, "Unable to load render texture id:%d for render model %s\n", pModel->diffuseTextureId, name);
    vr::VRRenderModels()->FreeRenderModel( pModel );
    return NULL; // move on to the next tracked device
    }

  auto md = new vr_rendermodel;
  vrdata.models.emplace_back(md);
  md->name = name;
  
  int cnt = pModel->unTriangleCount * 3;
  for(int i=0; i<cnt; i++) {
    glhr::textured_vertex tv;
    int id = pModel->rIndexData[i];
    for(int j=0; j<3; j++)
      tv.coords[j] = pModel->rVertexData[id].vPosition.v[j];
    tv.coords[3] = 1;
    for(int j=0; j<2; j++)
      tv.texture[j] = pModel->rVertexData[id].rfTextureCoord[j];
    md->vertices.push_back(tv);
    }

  glGenTextures(1, &md->texture_id);
  glBindTexture( GL_TEXTURE_2D, md->texture_id);

  glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, pTexture->unWidth, pTexture->unHeight,
    0, GL_RGBA, GL_UNSIGNED_BYTE, pTexture->rubTextureMapData );

  glGenerateMipmap(GL_TEXTURE_2D);

  glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
  glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
  glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
  glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );

  GLfloat fLargest;
  glGetFloatv( GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &fLargest );
  glTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, fLargest );

  glBindTexture( GL_TEXTURE_2D, 0 );
  
  println(hlog, "model loaded successfully");
  return md;
  }

#if HDR
struct click {
  int x, y, clicked;
  };
#endif

EX vector<click> get_hits() {
  vector<click> res;
  for(auto h: vrhr::vrdata.cdata)
    if(h.x || h.y)
      res.emplace_back(click{h.x, h.y, h.clicked});
  return res;
  }

void track_all() {
  track_actions();

  E4;
  // println(hlog, "tracking");
  vr::VRCompositor()->WaitGetPoses(vrdata.poses, vr::k_unMaxTrackedDeviceCount, NULL, 0 );
  // println(hlog, "poses received");
  
  for(int i=0; i<(int)vr::k_unMaxTrackedDeviceCount; i++) {
    auto& p = vrdata.poses[i];
    vrdata.device_models[i] = nullptr;
    if(!p.bPoseIsValid)
      continue;
    transmatrix T = vr_to_hr(p.mDeviceToAbsoluteTracking) * sm;
    
    // println(hlog, "found ", device_class_name(vrdata.vr->GetTrackedDeviceClass(i)), " at ", T);
    
    vrdata.pose_matrix[i] = T;

    if(i == vr::k_unTrackedDeviceIndex_Hmd) {
      hmd_at = inverse(T);
      if(first) hmd_ref_at = hmd_at, first = false;
      }
    
    auto& cd = vrdata.cdata[i];
    cd.x = cd.y = 0;

    if(vrdata.vr->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_Controller) {
      string mname = GetTrackedDeviceString(i, vr::Prop_RenderModelName_String );
      vrdata.device_models[i] = get_render_model(mname);
      
      /*
      cd.last = cd.cur;
      bool ok = vrdata.vr->GetControllerState(i, &cd.cur, sizeof(state));
      if(ok) {
        println(hlog, "pressed = ", color_t(cd.cur.ulButtonPressed), " touched = ", color_t(cd.cur.ulButtonTouched), " on ", i);
        for(int i=0; i<5; i++)
          if(cd.cur.rAxis[i].x || cd.cur.rAxis[i].y)
            println(hlog, "axis ", i, " = ", tie(cd.cur.rAxis[i].x, cd.cur.rAxis[i].y));
        }
      */

      hyperpoint h1 = sm * hmd_at * vrdata.pose_matrix[i] * sm * C0;
      hyperpoint h2 = sm * hmd_at * vrdata.pose_matrix[i] * sm * point31(0, 0, -0.01);
      ld p = ilerp(h1[2], h2[2], -ui_depth);
      hyperpoint px = lerp(h1, h2, p);
      px[0] /= ui_size;
      px[1] /= -ui_size;
      px[0] += current_display->xsize/2;
      px[1] += current_display->ysize/2;
      cd.x = px[0];
      cd.y = px[1];
      }
    }
    
  }

EX void vr_control() {
  if(!enabled || !vid.usingGL) {
    if(state) shutdown_vr();
    return;
    }
  if(enabled && vid.usingGL && !state && !failed) {
    start_vr();
    }
  if(state == 1) {
    track_all();
    }
  }

EX void be_33(transmatrix& T) {
  for(int i=0; i<3; i++) T[i][3] = T[3][i] = 0;
  T[3][3] = 1;
  }

EX void apply_movement(const transmatrix& rel) {
  hyperpoint h0 = IN_E4(inverse(rel) * C0);
  hyperpoint h = h0;
  for(int i=0; i<3; i++) h[i] /= -absolute_unit_in_meters;
  
  shift_view(h);
  transmatrix Rot = rel;
  be_33(Rot);
  rotate_view(Rot);
  }

EX void vr_shift() {
  if(first) return;
  rug::using_rugview urv;
  if(GDIM == 2) return;
       
  if(hsm == eHeadset::holonomy) {    
    apply_movement(IN_E4(hmd_at * inverse(hmd_ref_at)));
    hmd_ref_at = hmd_at;
    playermoved = false;
    if(!rug::rugged) optimizeview();
    }
  }

EX ld absolute_unit_in_meters = 3;

void move_according_to(vr::ETrackedControllerRole role, bool last, bool cur) {
  if(!last && !cur) return;
  int id = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(role);
  if(id >= 0 && id < int(vr::k_unMaxTrackedDeviceCount)) {
    hyperpoint h;
    if(true) {
      E4;
      transmatrix T = (hsm == eHeadset::none ? hmd_at : hmd_ref_at) * vrdata.pose_matrix[id] * sm;
      vrhr::be_33(T);
      h = T * point31(0, 0, -0.01);
      }
    if(last && !cur)
      movevrdir(h);
    else {
      movedir md = vectodir(h);
      cellwalker xc = cwt + md.d + wstep;
      forward_cell = xc.at;
      }
    }
  }

struct digital_action_data {
  string action_name;
  vr::VRActionHandle_t handle;
  bool last, curr;
  function<void(bool, bool)> act;
  bool_reaction_t when;
  digital_action_data(string s, bool_reaction_t when, function<void(bool, bool)> f) : when(when) { action_name = s; act = f; handle = vr::k_ulInvalidActionHandle; }
  };

struct analog_action_data {
  string action_name;
  vr::VRActionHandle_t handle;
  ld x, y;
  function<void(ld, ld)> act;
  analog_action_data(string s, function<void(ld, ld)> f) { action_name = s; act = f; handle = vr::k_ulInvalidActionHandle; }
  };

struct set_data {
  string set_name;
  int prio;
  vr::VRActionHandle_t handle;
  bool_reaction_t when;
  set_data(string s, int p, bool_reaction_t w) { set_name = s; prio = p; when = w; handle = vr::k_ulInvalidActionHandle; }
  };

vector<digital_action_data> dads = {
  digital_action_data("/actions/menu/in/SelectLeft", [] { return !(cmode && sm::NORMAL); }, [] (bool last, bool curr) {
    if(curr && !last) { 
      int id = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole( vr::TrackedControllerRole_LeftHand);
      if(id >= 0 && id < int(vr::k_unMaxTrackedDeviceCount))
        vrdata.cdata[id].clicked = true;
      }
    }),
  digital_action_data("/actions/menu/in/SelectRight", [] { return !(cmode && sm::NORMAL); }, [] (bool last, bool curr) {
    if(curr && !last) { 
      int id = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole( vr::TrackedControllerRole_RightHand);
      if(id >= 0 && id < int(vr::k_unMaxTrackedDeviceCount))
        vrdata.cdata[id].clicked = true;
      }
    }),
  digital_action_data("/actions/menu/in/Exit", [] { return !(cmode && sm::NORMAL); }, [] (bool last, bool curr) {
    if(curr && !last) dialog::queue_key(PSEUDOKEY_EXIT);
    }),
  digital_action_data("/actions/game/in/MoveLeft", [] { return (cmode && sm::NORMAL); }, [] (bool last, bool curr) {
    move_according_to(vr::TrackedControllerRole_LeftHand, last, curr);
    }),
  digital_action_data("/actions/game/in/MoveRight", [] { return (cmode && sm::NORMAL); }, [] (bool last, bool curr) {
    move_according_to(vr::TrackedControllerRole_RightHand, last, curr);
    }),
  digital_action_data("/actions/game/in/Drop", [] { return (cmode && sm::NORMAL); }, [] (bool last, bool curr) {
    if(curr && !last) dialog::queue_key('g');
    }),
  digital_action_data("/actions/game/in/Skip turn", [] { return (cmode && sm::NORMAL); }, [] (bool last, bool curr) {
    if(curr && !last) dialog::queue_key('s');
    }),
  digital_action_data("/actions/game/in/EnterMenu", [] { return (cmode && sm::NORMAL); }, [] (bool last, bool curr) {
    if(curr && !last) dialog::queue_key(PSEUDOKEY_MENU);
    }),
  digital_action_data("/actions/general/in/SetReference", [] { return true; }, [] (bool last, bool curr) {
    if(curr && !last) hmd_ref_at = hmd_at;
    })
  };

vector<analog_action_data> aads = {
  analog_action_data("/actions/general/in/MoveCamera", [] (ld x, ld y) {
    vrgo_x = x;
    vrgo_y = y;
    }),
  analog_action_data("/actions/general/in/RotateCamera", [] (ld x, ld y) {
    vraim_x = x;
    vraim_y = y;
    }),
  };

vector<set_data> sads = {
  set_data("/actions/menu", 20, [] { return !(cmode & sm::NORMAL); }),
  set_data("/actions/game", 20, [] { return cmode & sm::NORMAL; }),
  set_data("/actions/general", 10, [] { return true; })
  };

void init_input() {
  const auto& vi = vr::VRInput();

  string cwd;
  
  char cwdbuf[PATH_MAX];
  if (getcwd(cwdbuf, sizeof(cwdbuf)) != NULL) {
    cwd = cwdbuf;
    println(hlog, "Found cwd: ", cwd);
    if(cwd.back() == '/' || cwd.back() == '\\') ;
    else cwd += (ISWINDOWS ? '\\' : '/');
    cwd += "hypervr_actions.json";
    }

  vi->SetActionManifestPath(cwd.c_str());
  
  for(auto& sad: sads)
    vi->GetActionSetHandle(sad.set_name.c_str(), &sad.handle);

  for(auto& dad: dads)
    vi->GetActionHandle(dad.action_name.c_str(), &dad.handle);

  for(auto& aad: aads)
    vi->GetActionHandle(aad.action_name.c_str(), &aad.handle);
  }

EX void track_actions() {

  for(auto& cd: vrdata.cdata)
    cd.clicked = false;
  
  forward_cell = nullptr;

  vector<vr::VRActiveActionSet_t> sets;
  
  for(auto& sad: sads) if(sad.when()) {
    sets.emplace_back();
    auto& s = sets.back();
    s.ulActionSet = sad.handle;
    s.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
    s.ulSecondaryActionSet = vr::k_ulInvalidInputValueHandle;
    s.nPriority = sad.prio;
    }

  if(isize(sets))
    vr::VRInput()->UpdateActionState( &sets[0], sizeof(vr::VRActiveActionSet_t), isize(sets));
  
  for(auto& dad: dads) {
    if(!dad.when()) continue;
    vr::InputDigitalActionData_t actionData;    
    vr::VRInput()->GetDigitalActionData(dad.handle, &actionData, sizeof(actionData), vr::k_ulInvalidInputValueHandle );
    dad.last = dad.curr;
    dad.curr = actionData.bState;
    dad.act(dad.last, dad.curr);
    }
  
  for(auto& aad: aads) {
    vr::InputAnalogActionData_t actionData;
    vr::VRInput()->GetAnalogActionData(aad.handle, &actionData, sizeof(actionData), vr::k_ulInvalidInputValueHandle );
    aad.x = actionData.x;
    aad.y = actionData.y;
    aad.act(aad.x, aad.y);
    }  
  }

EX void start_vr() {

  if(true) { sm = Id; sm[1][1] = sm[2][2] = -1; }

  vr::EVRInitError eError = vr::VRInitError_None;
  vrdata.vr = vr::VR_Init( &eError, vr::VRApplication_Scene );

  if(eError != vr::VRInitError_None) {
    error_msg = vr::VR_GetVRInitErrorAsEnglishDescription( eError );
    println(hlog, "Unable to init VR: ", error_msg);
    failed = true;
    return;
    }  
  else println(hlog, "VR initialized successfully");

  string driver = GetTrackedDeviceString( vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_TrackingSystemName_String );
  string display = GetTrackedDeviceString( vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SerialNumber_String );

  println(hlog, "HyperRogue VR: driver=", driver, " display=", display);
  
  if(!vr::VRCompositor()) {
    println(hlog,  "Compositor initialization failed. See log file for details\n" );
    exit(1);
    }
  
  init_input();
  
  vrdata.vr->GetRecommendedRenderTargetSize( &vrdata.xsize, &vrdata.ysize);
  
  println(hlog, "recommended size: ", int(vrdata.xsize), " x ", int(vrdata.ysize));
  
  for(int a=0; a<2; a++) {
    auto eye = vr::EVREye(a);
    vrdata.eyes[a] = new vr_framebuffer(vrdata.xsize, vrdata.ysize);
    println(hlog, "eye ", a, " : ", vrdata.eyes[a]->ok ? "OK" : "Error");
    
    vrdata.proj[a] = 
      vr_to_hr(vrdata.vr->GetProjectionMatrix(eye, 0.01, 300));
    
    println(hlog, "projection = ", vrdata.proj[a]);
    
    vrdata.eyepos[a] =
      vr_to_hr(vrdata.vr->GetEyeToHeadTransform(eye));

    println(hlog, "eye-to-head = ", vrdata.eyepos[a]);
    }
  
  //CreateFrameBuffer( m_nRenderWidth, m_nRenderHeight, leftEyeDesc );
  //CreateFrameBuffer( m_nRenderWidth, m_nRenderHeight, rightEyeDesc );
  
  state = 1;
  }

EX void shutdown_vr() {
  vr::VR_Shutdown();
  vrdata.vr = nullptr;
  for(auto& e: vrdata.eyes) {
    delete e;
    e = nullptr;
    }
  state = 0;
  }

EX void clear() {
  if(!state) return;
  resetbuffer rb;
  for(int i=0; i<2; i++) {
    auto& ey = vrdata.eyes[i];
    glBindFramebuffer( GL_FRAMEBUFFER, ey->m_nRenderFramebufferId );
    glViewport(0, 0, vrdata.xsize, vrdata.ysize );
    glhr::set_depthtest(false);
    glhr::set_depthtest(true);
    glhr::set_depthwrite(false);
    glhr::set_depthwrite(true);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
  rb.reset();
  current_display->set_viewport(0);
  }

EX ld ui_depth = 1.5;
EX ld ui_size = 0.004;

EX void in_vr_ui(reaction_t what) {
  
  resetbuffer rb;
  if(!state) return;

  int xsi = current_display->xsize;
  int ysi = current_display->ysize;
  state = 2;

  for(int i=0; i<2; i++) {
    dynamicval<int> vx(vid.xres, vrdata.xsize);
    dynamicval<int> vy(vid.yres, vrdata.ysize);
    E4;
    auto& ey = vrdata.eyes[i];
    glBindFramebuffer( GL_FRAMEBUFFER, ey->m_nRenderFramebufferId );
    glViewport(0, 0, vrdata.xsize, vrdata.ysize );
    calcparam();
    glhr::set_depthtest(false);
    hmd_mvp = Id;
    hmd_mvp = xpush(-xsi/2) * ypush(-ysi/2) * hmd_mvp;
    transmatrix Sca = Id;
    Sca[0][0] *= ui_size;
    Sca[1][1] *= -ui_size;
    Sca[2][2] *= 0;
    hmd_mvp = Sca * hmd_mvp;
    hmd_mvp = zpush(-ui_depth) * hmd_mvp;
    hmd_mvp = vrdata.proj[i] * inverse(vrdata.eyepos[i]) * hmd_mvp;
    reset_projection();
    current_display->set_all(0, 0);
    what();
    }
  state = 1;

  rb.reset();
  calcparam();
  current_display->set_viewport(0);
  calcparam();
  reset_projection();
  current_display->set_all(0, 0);
  glhr::set_modelview(glhr::translate(-current_display->xcenter,-current_display->ycenter, 0));
  what();
  }

EX void draw_eyes() {
  state = 1;
  for(int i=0; i<2; i++) {
    resetbuffer rb;
    auto& ey = vrdata.eyes[i];
    glBindFramebuffer(GL_READ_FRAMEBUFFER, ey->m_nRenderFramebufferId);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ey->m_nResolveFramebufferId );
    glBlitFramebuffer( 0, 0, vrdata.xsize, vrdata.ysize, 0, 0, vrdata.xsize, vrdata.ysize, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    rb.reset();

    current_display->next_shader_flags = GF_TEXTURE;
    dynamicval<eModel> m(pmodel, mdPixel);
    current_display->set_all(0, 0);
    glBindTexture(GL_TEXTURE_2D, ey->m_nResolveTextureId );
    glhr::id_modelview();
    glhr::set_depthtest(false);
    glhr::color2(0xFFFFFFFF);
    vector<glhr::textured_vertex> tvx;
    for(int a=0; a<6; a++) {
      int dx[6] = {0, 1, 1, 0, 0, 1};
      int dy[6] = {0, 0, 1, 0, 1, 1};
      glhr::textured_vertex tx;
      tx.coords[2] = 0;
      tx.coords[3] = 1;
      tx.coords[0] = (dx[a]+i) * current_display->xsize / 2 - current_display->xcenter;
      tx.coords[1] = (1-dy[a]) * current_display->ysize - current_display->ycenter;
      tx.texture[0] = dx[a];
      tx.texture[1] = dy[a];
      tvx.push_back(tx);
      }
    glhr::prepare(tvx);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    }
  }

EX void render() {

  resetbuffer rb;
  state = 2;
  
  if(GDIM == 2) {
    state = 3;
    drawqueue();
    return;
    }
  
  // eyes = lshiftclick ? eEyes::truesim : eEyes::equidistant;
  
  // cscr = lshiftclick ? eCompScreen::eyes : eCompScreen::single;

  for(int i=0; i<2; i++) {

    dynamicval<int> vx(vid.xres, vrdata.xsize);
    dynamicval<int> vy(vid.yres, vrdata.ysize);
    
    auto& ey = vrdata.eyes[i];

    glBindFramebuffer( GL_FRAMEBUFFER, ey->m_nRenderFramebufferId );
    glViewport(0, 0, vrdata.xsize, vrdata.ysize );
    glhr::set_depthtest(false);
    glhr::set_depthtest(true);
    glhr::set_depthwrite(false);
    glhr::set_depthwrite(true);
    // glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    calcparam();
    
    transmatrix mu;
    for(int i=0; i<4; i++)
    for(int j=0; j<4; j++)
      mu[i][j] = i!=j ? 0 : i==3 ? 1 : absolute_unit_in_meters;

    if(1) {
      make_actual_view();
      shiftmatrix Tv = cview();
      dynamicval<transmatrix> tN(NLP, NLP);
      dynamicval<transmatrix> tV(View, View);
      dynamicval<transmatrix> tC(current_display->which_copy, current_display->which_copy);
      
      if(hsm == eHeadset::rotation_only) {
        transmatrix T = hmd_at;
        be_33(T);
        apply_movement(T);
        }
      
      else if(hsm == eHeadset::reference) {
        apply_movement(IN_E4(hmd_at * inverse(hmd_ref_at)));
        }

      if(eyes == eEyes::truesim) {
        apply_movement(IN_E4(inverse(vrdata.eyepos[i])));
        }

      make_actual_view();
      hmd_pre = cview().T * inverse(Tv.T);
      // inverse_shift(Tv, cview());
      // View * inverse(Tv.T);
      // inverse(inverse_shift(cview(), Tv));
      
      hmd_mvp = Id;
      bool nlpu = nisot::local_perspective_used();
      if(1) {
        E4;
        if(nlpu) {
          be_33(NLP);
          hmd_mvp = NLP * hmd_mvp;          
          }
        hmd_mvp = mu * sm * hmd_mvp;
        if(eyes == eEyes::equidistant) {
          hmd_mvp = inverse(vrdata.eyepos[i]) * hmd_mvp;
          }
        hmd_mvp = vrdata.proj[i] * hmd_mvp;
        }
      }
    
    drawqueue();
    }

  rb.reset();

  calcparam();
  current_display->set_viewport(0);
  calcparam();
  current_display->next_shader_flags = 0;
  current_display->set_all(0, 0);
  
  if(cscr == eCompScreen::eyes) draw_eyes();
  
  if(cscr == eCompScreen::single) {
    /* todo */
    state = 3;
    drawqueue();
    }

  if(cscr == eCompScreen::reference) {
    state = 3;
    drawqueue();
    }

  state = 1;  
  }

template<class T> 
void show_choice(string name, T& value, char key, vector<pair<string, string>> options) {
  dialog::addSelItem(XLAT(name), XLAT(options[int(value)].first), key);
  dialog::add_action_push([&value, name, options] {
    dialog::init(XLAT(name));
    dialog::addBreak(100);
    int q = isize(options);
    for(int i=0; i<q; i++) {
      dialog::addBoolItem(XLAT(options[i].first), int(value) == i, 'a'+i);
      dialog::add_action([&value, i] { value = T(i); popScreen(); });
      dialog::addBreak(100);
      dialog::addHelp(XLAT(options[i].second));
      dialog::addBreak(100);
      }
    dialog::addBreak(100);
    dialog::addBack();
    dialog::display();
    });
  }

EX void show_vr_settings() {
  cmode = sm::SIDE | sm::MAYDARK;
  gamescreen(0);
  dialog::init(XLAT("VR settings"));
  
  dialog::addBoolItem_action(XLAT("VR enabled"), enabled, 'o');
  if(!enabled)
    dialog::addBreak(100);
  else if(failed)
    dialog::addInfo(XLAT("error: ") + error_msg, 0xC00000);
  else
    dialog::addInfo(XLAT("VR initialized correctly"), 0x00C000);
  
  dialog::addBreak(100);

  show_choice("headset movement", hsm, 'h', headset_desc);
  show_choice("binocular vision", eyes, 'b', eyes_desc);
  show_choice("computer screen", cscr, 'c', comp_desc);
  
  dialog::addSelItem(XLAT("absolute unit in meters"), fts(absolute_unit_in_meters), 'a');
  dialog::add_action([] {
    dialog::editNumber(absolute_unit_in_meters, .01, 100, 0.1, 1, XLAT("absolute unit in meters"), 
      XLAT(
        "The size of the absolute unit of the non-Euclidean geometry correspond in meters. "
        "This affects the headset movement and binocular vision.\n\n"
        "In spherical geometry, the absolute unit is the radius of the sphere. "
        "The smaller the absolute unit, the stronger the non-Euclidean effects.\n\n"
        "Elements of the HyperRogue world have fixed size in terms of absolute units, "
        "so reducing the absolute unit makes them smaller. "
        "If you are playing in the Euclidean mode, this feature just scales everything "
        "(e.g., in the cube tiling, the 'absolute unit' is just the edge of the cube)."
        ));
      dialog::scaleLog();
      });
    
  if(hsm == eHeadset::reference) {
    hyperpoint h = hmd_at * inverse(hmd_ref_at) * C0;
      
    dialog::addSelItem(XLAT("reset the reference point"), state ? fts(hypot_d(3, h)) + "m" : "", 'r');
    dialog::add_action([] { hmd_ref_at = hmd_at; });
    }
  else dialog::addBreak(100);
  
  dialog::addBack();
  dialog::display();
  }

#if CAP_COMMANDLINE
int readArgs() {
  using namespace arg;
           
  if(0) ;
  else if(argis("-vr-enabled")) {
    PHASEFROM(2);
    shift(); enabled = argi();
    }
  else if(argis("-vr-absunit")) {
    PHASEFROM(2);
    shift_arg_formula(absolute_unit_in_meters);
    }
  else if(argis("-d:vr")) {
    PHASEFROM(2); launch_dialog(show_vr_settings);
    }
  else if(argis("-vr-mode")) {
    PHASEFROM(2); 
    shift(); hsm = (eHeadset) argi();
    shift(); eyes = (eEyes) argi();
    shift(); cscr = (eCompScreen) argi();
    }
  else return 1;
  return 0;
  }
auto hooka = addHook(hooks_args, 100, readArgs);
#endif

#if CAP_CONFIG
void addconfig() {
  addsaver(enabled, "vr-enabled");
  addparam(absolute_unit_in_meters, "vr-abs-unit");
  }
auto hookc = addHook(hooks_configfile, 100, addconfig);
#endif

EX void submit() {

  if(!state) return;
  for(int i=0; i<(int)vr::k_unMaxTrackedDeviceCount; i++) 
    if(vrdata.device_models[i]) {
      resetbuffer rb;
      if(!state) return;
    
      state = 2;
      dynamicval<eModel> m(pmodel, mdPerspective);
      dynamicval<ld> ms(sightranges[geometry], 100);

      for(int e=0; e<2; e++) {
        dynamicval<int> vx(vid.xres, vrdata.xsize);
        dynamicval<int> vy(vid.yres, vrdata.ysize);
        E4;
        auto& ey = vrdata.eyes[e];
        glBindFramebuffer( GL_FRAMEBUFFER, ey->m_nRenderFramebufferId );
        glViewport(0, 0, vrdata.xsize, vrdata.ysize );
        calcparam();

        hmd_mvp = vrdata.proj[e] * inverse(vrdata.eyepos[e]) * sm * hmd_at * vrdata.pose_matrix[i] * sm * Id;
        hmd_pre = Id;
        
        reset_projection();        
        current_display->next_shader_flags = GF_TEXTURE;
        current_display->set_all(0, 0);
        glhr::set_depthtest(false);
        glhr::set_depthtest(true);
        glhr::set_depthwrite(false);
        glhr::set_depthwrite(true);
        glClear(GL_DEPTH_BUFFER_BIT);
        glhr::id_modelview();
        glhr::color2(0xFFFFFFFF);
        prepare(vrdata.device_models[i]->vertices);
        
        glBindTexture(GL_TEXTURE_2D, vrdata.device_models[i]->texture_id);
        glDrawArrays(GL_TRIANGLES, 0, isize(vrdata.device_models[i]->vertices));

        if(1) {
          current_display->next_shader_flags = 0;
          current_display->set_all(0, 0);
          vector<glvertex> vex;
          vex.push_back(glhr::makevertex(0.01, 0, 0));
          vex.push_back(glhr::makevertex(-0.01, 0, 0));
          vex.push_back(glhr::makevertex(0, 0, -10));
          glhr::current_vertices = nullptr;
          glhr::vertices(vex);
          glhr::color2(0xC0FFC0C0);
          glDrawArrays(GL_TRIANGLES, 0, 3);
          }

        }

      state = 1;
      
      rb.reset();
      calcparam();
      current_display->set_viewport(0);
      calcparam();
      reset_projection();
      current_display->set_all(0, 0);
      }    
    
  for(int i=0; i<2; i++) {
    auto eye = vr::EVREye(i);
    auto& ey = vrdata.eyes[i];
        
    resetbuffer rb;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, ey->m_nRenderFramebufferId);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ey->m_nResolveFramebufferId );
    glBlitFramebuffer( 0, 0, vrdata.xsize, vrdata.ysize, 0, 0, vrdata.xsize, vrdata.ysize, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    rb.reset();

    vr::Texture_t texture = {(void*)(uintptr_t)ey->m_nResolveTextureId, vr::TextureType_OpenGL, vr::ColorSpace_Gamma };
    vr::VRCompositor()->Submit(eye, &texture );
    }
  }

#endif

EX }
}
