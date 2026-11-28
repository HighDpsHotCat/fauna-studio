/* Cocoa / SceneKit bindings only. Model processing and exports are C17. */
#import <Cocoa/Cocoa.h>
#import <SceneKit/SceneKit.h>
#include "fauna.h"
#include <math.h>

static NSString *stringW(const wchar_t *s){char b[FS_PATH*4];return fs_encode_utf8(0,0,s,-1,b,sizeof(b),NULL,NULL)?[NSString stringWithUTF8String:b]:@"";}
static int pathWide(NSString *s,wchar_t *out){return fs_decode_utf8(0,0,s.UTF8String,-1,out,FS_PATH)>0;}
static NSTextField *label(NSString *s,BOOL bold){NSTextField *v=[NSTextField labelWithString:s];v.font=bold?[NSFont boldSystemFontOfSize:15]:[NSFont systemFontOfSize:13];return v;}
static NSStackView *stack(NSArray<NSView*> *views,NSUserInterfaceLayoutOrientation direction){NSStackView *s=[NSStackView stackViewWithViews:views];s.orientation=direction;s.spacing=10;s.alignment=direction==NSUserInterfaceLayoutOrientationVertical?NSLayoutAttributeLeading:NSLayoutAttributeCenterY;return s;}
static void fixedHeight(NSView *v,CGFloat h){[v.heightAnchor constraintEqualToConstant:h].active=YES;}
static void fullWidth(NSView *v,NSView *parent){[v.widthAnchor constraintEqualToAnchor:parent.widthAnchor].active=YES;}

@interface Studio : NSObject <NSApplicationDelegate,NSWindowDelegate> {
    NSWindow *_window;NSImageView *_photo;SCNView *_viewer;NSTextField *_filename,*_style,*_mm,*_status,*_count;
    NSPopUpButton *_quality;NSButton *_references,*_generate,*_cancel,*_save,*_open,*_browse,*_example,*_folder;
    NSArray<NSButton*> *_formats;NSString *_input,*_lastOutput;wchar_t _root[FS_PATH];Mesh _mesh;
    BOOL _busy,_terminateAfterJob;FsCancel *_cancelToken;
}
- (void)progress:(NSString*)message;
@end

static void progress(void *context,const wchar_t *message){
    Studio *studio=(__bridge Studio*)context;NSString *s=stringW(message);
    dispatch_async(dispatch_get_main_queue(),^{[studio progress:s];});
}

@implementation Studio
- (NSButton*)button:(NSString*)title action:(SEL)action {return [NSButton buttonWithTitle:title target:self action:action];}
- (void)alert:(NSString*)title text:(NSString*)text {NSAlert *a=[NSAlert new];a.messageText=title;a.informativeText=text;[a beginSheetModalForWindow:_window completionHandler:nil];}
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;fs_init();
    if(!fs_root(_root)){NSAlert *a=[NSAlert new];a.messageText=@"프로젝트 폴더를 찾지 못했습니다.";a.informativeText=@"프로젝트 폴더에서 run.command를 실행하세요.";[a runModal];[NSApp terminate:nil];return;}
    NSMenu *menu=[NSMenu new];NSMenuItem *item=[NSMenuItem new];[menu addItem:item];NSMenu *app=[NSMenu new];
    [app addItemWithTitle:@"AAAnimal Studio 종료" action:@selector(terminate:) keyEquivalent:@"q"];item.submenu=app;NSApp.mainMenu=menu;
    _window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1180,800) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
    _window.title=@"AAAnimal Studio";_window.minSize=NSMakeSize(960,780);_window.delegate=self;_window.releasedWhenClosed=NO;
    _photo=[NSImageView new];_photo.imageScaling=NSImageScaleProportionallyUpOrDown;fixedHeight(_photo,178);
    _filename=label(@"사진을 선택하세요",NO);_filename.lineBreakMode=NSLineBreakByTruncatingMiddle;
    _browse=[self button:@"사진 첨부…" action:@selector(browse:)];_example=[self button:@"예제 사진" action:@selector(example:)];
    NSButton *qualityHelp=[self button:@"?" action:@selector(qualityHelp:)];qualityHelp.bezelStyle=NSBezelStyleHelpButton;qualityHelp.toolTip=@"자동 로우폴리와 품질 안내";
    _style=label(@"각진 로우폴리 · 자동",NO);
    _quality=[NSPopUpButton new];[_quality addItemsWithTitles:@[@"빠르게 · 192",@"고품질 · 256",@"세밀하게 · 320"]];[_quality selectItemAtIndex:2];
    _references=[NSButton checkboxWithTitle:@"유사 자세 참고로 형태 보완" target:nil action:nil];_references.state=NSControlStateValueOn;
    NSButton *formatHelp=[self button:@"?" action:@selector(formatHelp:)];formatHelp.bezelStyle=NSBezelStyleHelpButton;formatHelp.toolTip=@"저장 형식 안내";
    NSMutableArray *formats=[NSMutableArray array];for(NSString *name in @[@"GLB",@"STL",@"STEP",@"3MF"])[formats addObject:[NSButton checkboxWithTitle:name target:nil action:nil]];
    _formats=formats;_formats[0].state=NSControlStateValueOn;
    _mm=[NSTextField textFieldWithString:@"100"];[_mm.widthAnchor constraintEqualToConstant:72].active=YES;
    _generate=[self button:@"3D 생성하기" action:@selector(generate:)];_generate.keyEquivalent=@"\r";
    _cancel=[self button:@"작업 취소" action:@selector(cancel:)];_cancel.enabled=NO;
    _status=label(@"준비됨",NO);_status.maximumNumberOfLines=3;_status.lineBreakMode=NSLineBreakByWordWrapping;fixedHeight(_status,64);
    _folder=[self button:@"저장 폴더 열기" action:@selector(folder:)];_folder.enabled=NO;
    NSStackView *left=stack(@[stack(@[label(@"사진 입력",YES),_example],NSUserInterfaceLayoutOrientationHorizontal),_photo,_browse,_filename,
        stack(@[label(@"모델 스타일 / 품질",YES),qualityHelp],NSUserInterfaceLayoutOrientationHorizontal),
        stack(@[_style,_quality],NSUserInterfaceLayoutOrientationHorizontal),_references,
        stack(@[label(@"저장 형식",YES),formatHelp],NSUserInterfaceLayoutOrientationHorizontal),stack(_formats,NSUserInterfaceLayoutOrientationHorizontal),
        stack(@[label(@"인쇄 크기 · 긴 변 (mm)",NO),_mm],NSUserInterfaceLayoutOrientationHorizontal),_generate,_cancel,_status,_folder],NSUserInterfaceLayoutOrientationVertical);
    [left.widthAnchor constraintEqualToConstant:286].active=YES;
    for(NSView *v in @[_photo,_browse,_filename,_generate,_cancel,_status,_folder])fullWidth(v,left);
    _save=[self button:@"선택 형식으로 저장" action:@selector(save:)];_save.enabled=NO;_open=[self button:@"GLB 열기…" action:@selector(open:)];
    NSView *space=[NSView new];[space setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *bar=stack(@[label(@"3D 미리 보기",YES),space,_save,_open],NSUserInterfaceLayoutOrientationHorizontal);
    _viewer=[[SCNView alloc] initWithFrame:NSZeroRect options:nil];_viewer.backgroundColor=NSColor.controlBackgroundColor;_viewer.allowsCameraControl=YES;_viewer.autoenablesDefaultLighting=NO;_viewer.antialiasingMode=SCNAntialiasingModeMultisampling4X;
    _count=label(@"",NO);NSStackView *right=stack(@[bar,_viewer,_count],NSUserInterfaceLayoutOrientationVertical);
    fixedHeight(bar,32);[_viewer.heightAnchor constraintGreaterThanOrEqualToConstant:300].active=YES;
    [_viewer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];
    fullWidth(bar,right);fullWidth(_viewer,right);fixedHeight(_count,20);
    NSView *content=_window.contentView;left.translatesAutoresizingMaskIntoConstraints=NO;right.translatesAutoresizingMaskIntoConstraints=NO;
    [content addSubview:left];[content addSubview:right];
    [NSLayoutConstraint activateConstraints:@[[left.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:20],[left.topAnchor constraintEqualToAnchor:content.topAnchor constant:20],
        [left.bottomAnchor constraintLessThanOrEqualToAnchor:content.bottomAnchor constant:-20],
        [right.leadingAnchor constraintEqualToAnchor:left.trailingAnchor constant:24],[right.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-20],
        [right.topAnchor constraintEqualToAnchor:content.topAnchor constant:20],[right.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-20]]];
    [self displayMesh];[_window center];[_window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];
    if(getenv("AAANIMAL_UI_SMOKE"))dispatch_after(dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC),dispatch_get_main_queue(),^{[self smokeTest];});
}
- (void)smokeTest {
    wchar_t path[FS_PATH],error[FS_ERROR]={0};fs_join(path,_root,L"examples/red-fox-lowpoly-3000.glb");
    if(!mesh_read_glb(path,&_mesh,error)){fprintf(stderr,"UI smoke: example load failed\n");exit(1);}
    [self example:nil];[self displayMesh];[_window.contentView layoutSubtreeIfNeeded];
    if(_viewer.bounds.size.width<500||_viewer.bounds.size.height<300||!_viewer.scene||_mesh.triangle_count!=3000){fprintf(stderr,"UI smoke: invalid layout or scene\n");exit(1);}
    const char *output=getenv("AAANIMAL_UI_SMOKE");
    NSBitmapImageRep *bitmap=[_window.contentView bitmapImageRepForCachingDisplayInRect:_window.contentView.bounds];
    [_window.contentView cacheDisplayInRect:_window.contentView.bounds toBitmapImageRep:bitmap];
    /* cacheDisplay omits Metal layers and the NSWindow background. */
    NSImage *capture=[[NSImage alloc] initWithSize:_window.contentView.bounds.size];[capture lockFocus];
    [NSColor.windowBackgroundColor setFill];NSRectFill(_window.contentView.bounds);[bitmap drawInRect:_window.contentView.bounds fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:NO hints:nil];
    NSRect sceneRect=[_viewer convertRect:_viewer.bounds toView:_window.contentView];
    [[_viewer snapshot] drawInRect:sceneRect fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1];[capture unlockFocus];
    NSData *png=[[NSBitmapImageRep imageRepWithData:capture.TIFFRepresentation] representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    if(!png||![png writeToFile:[NSString stringWithUTF8String:output] atomically:YES]){fprintf(stderr,"UI smoke: capture failed\n");exit(1);}
    puts("PASS Cocoa window, controls, SceneKit scene and example model");[NSApp terminate:nil];
}
- (void)qualityHelp:(id)sender {(void)sender;[self alert:@"모델 스타일 / 품질" text:@"모든 동물에 같은 각진 로우폴리 스타일을 적용합니다.\n면 수는 프로그램이 자동으로 정합니다. 귀·주둥이·턱·발 등 큰 형태를 살리며 눈 무늬는 지웁니다. 얼굴 위치가 불확실하면 단색 털색을 사용합니다.\n품질은 사진에서 형상을 추출하는 정밀도입니다. 높이면 처리 시간이 늘어나며, 최종 스타일은 각진 로우폴리로 유지됩니다."];}
- (void)formatHelp:(id)sender {(void)sender;[self alert:@"저장 형식" text:@"여러 개를 선택할 수 있습니다. 생성된 모델은 다시 생성하지 않고 다른 형식으로 저장할 수 있습니다.\nGLB: 색상이 있는 3D 모델\nSTL: 색상 없는 삼각형 메시\nSTEP: 색상 없는 삼각면 기반 CAD 형상\n3MF: 크기·색상을 포함하는 3D 인쇄 파일\n3MF 색상 지원은 슬라이서·프린터에 따라 다릅니다. G-code는 슬라이서에서 만드세요."];}
- (void)setInput:(NSURL*)url {_input=url.path;_photo.image=[[NSImage alloc] initWithContentsOfURL:url];_filename.stringValue=url.lastPathComponent;}
- (void)browse:(id)sender {(void)sender;NSOpenPanel *p=[NSOpenPanel openPanel];p.allowedFileTypes=@[@"jpg",@"jpeg",@"png",@"webp",@"heic",@"tiff"];p.allowsMultipleSelection=NO;p.canChooseDirectories=NO;[p beginSheetModalForWindow:_window completionHandler:^(NSModalResponse result){if(result==NSModalResponseOK)[self setInput:p.URL];}];}
- (void)example:(id)sender {(void)sender;[self setInput:[NSURL fileURLWithPath:[stringW(_root) stringByAppendingPathComponent:@"assets/red-fox.jpg"]]];}
- (void)progress:(NSString*)message {_status.stringValue=message;}
- (void)setBusy:(BOOL)busy {
    _busy=busy;for(NSControl *c in @[_browse,_example,_open,_generate,_quality,_references,_mm])c.enabled=!busy;
    for(NSButton *c in _formats)c.enabled=!busy;_cancel.enabled=busy;_save.enabled=!busy&&_mesh.triangle_count>0;
}
- (Job*)job {
    Job *j=calloc(1,sizeof(*j));if(!j){[self alert:@"메모리 부족" text:@"다른 작업을 닫고 다시 시도하세요."];return NULL;}
    fs_wcopy(j->root,FS_PATH,_root);if(_input&&!pathWide(_input,j->input)){free(j);return NULL;}
    j->target=FS_FORM_BASE_TRIANGLES;j->resolution=(uint32_t[]){192,256,320}[_quality.indexOfSelectedItem];
    j->cad_size_mm=_mm.doubleValue;j->shape_disabled=_references.state!=NSControlStateValueOn;
    for(NSUInteger i=0;i<_formats.count;i++)if(_formats[i].state==NSControlStateValueOn)j->formats|=1u<<i;
    if(!j->formats||!isfinite(j->cad_size_mm)||j->cad_size_mm<1||j->cad_size_mm>10000){free(j);[self alert:@"입력을 확인하세요" text:@"저장 형식을 하나 이상 선택하세요. 인쇄 크기는 1~10,000 mm입니다."];return NULL;}
    j->cancel=fs_cancel_create(NULL,1,0,NULL);if(!j->cancel){free(j);return NULL;}j->progress=progress;j->context=(__bridge void*)self;return j;
}
- (void)work:(BOOL)generate {
    if(_busy)return;if(generate&&!_input){[self alert:@"사진을 선택하세요" text:@"사진 첨부 또는 예제 사진을 누르세요."];return;}
    Job *j=[self job];if(!j)return;_cancelToken=j->cancel;[self setBusy:YES];_status.stringValue=generate?@"생성 준비 중":@"선택한 형식으로 저장 중";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
        int ok=generate?fs_generate(j):fs_save_model(j,&self->_mesh);
        dispatch_async(dispatch_get_main_queue(),^{
            self->_cancelToken=NULL;fs_cancel_free(j->cancel);
            if(ok){if(generate){mesh_free(&self->_mesh);self->_mesh=j->preview;memset(&j->preview,0,sizeof(j->preview));[self displayMesh];}
                self->_lastOutput=stringW(j->output);self->_folder.enabled=YES;self->_status.stringValue=@"저장 완료";
            }else self->_status.stringValue=j->error[0]?stringW(j->error):@"처리에 실패했습니다. 저장 폴더의 로그를 확인하세요.";
            mesh_free(&j->preview);free(j);[self setBusy:NO];if(self->_terminateAfterJob)[NSApp replyToApplicationShouldTerminate:YES];
        });
    });
}
- (void)generate:(id)sender {(void)sender;[self work:YES];}
- (void)save:(id)sender {(void)sender;if(_mesh.triangle_count)[self work:NO];}
- (void)cancel:(id)sender {(void)sender;fs_cancel_set(_cancelToken);_cancel.enabled=NO;_status.stringValue=@"취소 중";}
- (void)folder:(id)sender {(void)sender;if(_lastOutput)[NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:_lastOutput]];}
- (void)open:(id)sender {
    (void)sender;NSOpenPanel *p=[NSOpenPanel openPanel];p.allowedFileTypes=@[@"glb"];p.canChooseDirectories=NO;p.allowsMultipleSelection=NO;
    [p beginSheetModalForWindow:_window completionHandler:^(NSModalResponse result){if(result!=NSModalResponseOK)return;
        wchar_t path[FS_PATH],error[FS_ERROR]={0};Mesh model={0};if(!pathWide(p.URL.path,path)||!mesh_read_glb(path,&model,error)){[self alert:@"GLB를 열지 못했습니다" text:stringW(error)];return;}
        mesh_free(&self->_mesh);self->_mesh=model;[self displayMesh];self->_save.enabled=YES;self->_status.stringValue=@"모델을 불러왔습니다";
    }];
}
- (void)displayMesh {
    SCNScene *scene=[SCNScene scene];float lo[3]={-.5f,-.5f,-.5f},hi[3]={.5f,.5f,.5f};
    if(_mesh.triangle_count){
        mesh_bounds(&_mesh,lo,hi);NSMutableData *vertices=[NSMutableData dataWithLength:(NSUInteger)_mesh.vertex_count*10*sizeof(float)];float *d=vertices.mutableBytes;
        for(uint32_t i=0;i<_mesh.vertex_count;i++){Vertex *v=&_mesh.vertices[i];memcpy(d+10*i,v->p,12);memcpy(d+10*i+3,v->n,12);for(int c=0;c<4;c++){float value=v->color[c]/255.0f;d[10*i+6+c]=c==3?value:(value<=.04045f?value/12.92f:powf((value+.055f)/1.055f,2.4f));}}
        NSMutableArray *sources=[NSMutableArray array];NSArray *semantics=@[SCNGeometrySourceSemanticVertex,SCNGeometrySourceSemanticNormal,SCNGeometrySourceSemanticColor];
        for(int i=0;i<3;i++)[sources addObject:[SCNGeometrySource geometrySourceWithData:vertices semantic:semantics[i] vectorCount:_mesh.vertex_count floatComponents:YES componentsPerVector:i==2?4:3 bytesPerComponent:sizeof(float) dataOffset:(NSUInteger)i*3*sizeof(float) dataStride:10*sizeof(float)]];
        NSData *indices=[NSData dataWithBytes:_mesh.triangles length:(NSUInteger)_mesh.triangle_count*sizeof(Triangle)];SCNGeometryElement *element=[SCNGeometryElement geometryElementWithData:indices primitiveType:SCNGeometryPrimitiveTypeTriangles primitiveCount:_mesh.triangle_count bytesPerIndex:sizeof(uint32_t)];
        SCNGeometry *geometry=[SCNGeometry geometryWithSources:sources elements:@[element]];SCNMaterial *material=[SCNMaterial material];material.diffuse.contents=NSColor.whiteColor;material.lightingModelName=SCNLightingModelLambert;material.doubleSided=YES;geometry.materials=@[material];[scene.rootNode addChildNode:[SCNNode nodeWithGeometry:geometry]];
    }
    float span=fmaxf(hi[0]-lo[0],fmaxf(hi[1]-lo[1],hi[2]-lo[2]));if(span<.001f)span=1;
    float cx=(lo[0]+hi[0])*.5f,cy=(lo[1]+hi[1])*.5f,cz=(lo[2]+hi[2])*.5f;
    float extent=span*1.25f,y=lo[1]-span*.003f;
    SCNMaterial *line=[SCNMaterial material];line.lightingModelName=SCNLightingModelConstant;
    line.diffuse.contents=[NSColor colorWithSRGBRed:.55 green:.57 blue:.59 alpha:1];
    for(int i=-10;i<=10;i++)for(int axis=0;axis<2;axis++){
        float t=extent*i/10,thickness=span*.001f;
        SCNBox *box=[SCNBox boxWithWidth:axis?extent*2:thickness height:thickness*.25f length:axis?thickness:extent*2 chamferRadius:0];box.materials=@[line];
        SCNNode *node=[SCNNode nodeWithGeometry:box];node.position=SCNVector3Make(cx+(axis?0:t),y,cz+(axis?t:0));[scene.rootNode addChildNode:node];
    }
    SCNNode *ambient=[SCNNode node];ambient.light=[SCNLight light];ambient.light.type=SCNLightTypeAmbient;ambient.light.intensity=250;[scene.rootNode addChildNode:ambient];
    SCNNode *light=[SCNNode node];light.light=[SCNLight light];light.light.type=SCNLightTypeOmni;light.light.intensity=600;light.position=SCNVector3Make(cx-span*2,cy+span*3,cz-span*4);[scene.rootNode addChildNode:light];
    SCNNode *camera=[SCNNode node];camera.camera=[SCNCamera camera];camera.camera.fieldOfView=38;camera.camera.zNear=span*.005;camera.camera.zFar=span*100;
    camera.position=SCNVector3Make(cx+span*.35f,cy+span*.22f,cz-span*2.35f);[camera lookAt:SCNVector3Make(cx,cy,cz)];[scene.rootNode addChildNode:camera];
    _viewer.scene=scene;_viewer.pointOfView=camera;_viewer.defaultCameraController.target=SCNVector3Make(cx,cy,cz);_viewer.defaultCameraController.inertiaEnabled=YES;
    _count.stringValue=_mesh.triangle_count?[NSString stringWithFormat:@"삼각형 %u개",_mesh.triangle_count]:@"";
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {(void)sender;if(_busy){_terminateAfterJob=YES;[self cancel:nil];return NSTerminateLater;}return NSTerminateNow;}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {(void)sender;return YES;}
- (BOOL)windowShouldClose:(NSWindow*)sender {(void)sender;if(_busy){[NSApp terminate:nil];return NO;}return YES;}
- (void)dealloc {mesh_free(&_mesh);}
@end

int main(int argc,const char **argv){(void)argc;(void)argv;@autoreleasepool {NSApplication *app=NSApplication.sharedApplication;app.activationPolicy=NSApplicationActivationPolicyRegular;Studio *studio=[Studio new];app.delegate=studio;[app run];}return 0;}
