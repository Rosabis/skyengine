/* macOS 原生菜单栏。AppKit 的 NSMenu 挂在屏幕顶部(所有 Mac 应用的标准位置),
 * 不占用 SDL 客户区,因此不会污染 PPM/e2e 像素。菜单动作只投递命令码,
 * 真正的开文件/重启仍在 SDL 主循环里执行。 */
#import <Cocoa/Cocoa.h>
#include "./include/desktop_shell_internal.h"

#include <string.h>
#include <stdio.h>

@interface SkyEngineMenuTarget : NSObject
- (void)onCommand:(id)sender;
@end

@implementation SkyEngineMenuTarget
- (void)onCommand:(id)sender {
    desktop_shell_queue_cmd((int)[sender tag]);
}
@end

@interface SkyEngineAdvancedPanel : NSObject
@property (strong) NSTextField *mrpField;
- (void)browse:(id)sender;
@end

@implementation SkyEngineAdvancedPanel
- (void)browse:(id)sender {
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setAllowedFileTypes:@[@"mrp"]];
    [panel setCanChooseFiles:YES];
    [panel setCanChooseDirectories:NO];
    if ([panel runModal] == NSModalResponseOK) {
        NSURL *url = [[panel URLs] firstObject];
        if (url) [self.mrpField setStringValue:[url path]];
    }
}
@end

static SkyEngineMenuTarget *g_target;
static NSMenu *g_recent_menu;
static NSMenuItem *g_file_item;
static NSMenuItem *g_settings_item;

static NSString *ns_utf8(const char *s) {
    return s ? [NSString stringWithUTF8String:s] : @"";
}

static int copy_utf8(NSString *s, char *out, size_t n) {
    if (!out || n == 0) return -1;
    const char *u = s ? [s UTF8String] : "";
    if (!u) u = "";
    snprintf(out, n, "%s", u);
    return 0;
}

static NSMenuItem *add_item(NSMenu *menu, const char *title, int cmd, NSString *key) {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:ns_utf8(title)
                                                  action:@selector(onCommand:)
                                           keyEquivalent:(key ? key : @"")];
    [item setTarget:g_target];
    [item setTag:cmd];
    [menu addItem:item];
    return item;
}

int desktop_shell_cocoa_init(void) {
    @autoreleasepool {
        if (![NSThread isMainThread]) return -1;
        g_target = [[SkyEngineMenuTarget alloc] init];

        NSMenu *menubar = [NSApp mainMenu];
        if (!menubar) {
            menubar = [[NSMenu alloc] initWithTitle:@""];
            [NSApp setMainMenu:menubar];
        }

        /* 插在应用菜单之后:macOS 第一项必须是 App 菜单(关于/隐藏/退出)。 */
        NSInteger insert_at = [menubar numberOfItems] > 0 ? 1 : 0;

        NSMenuItem *file_item = [[NSMenuItem alloc] initWithTitle:@"文件" action:nil keyEquivalent:@""];
        NSMenu *file_menu = [[NSMenu alloc] initWithTitle:@"文件"];
        add_item(file_menu, "选择 MRP 启动...", CMD_OPEN, @"o");
        add_item(file_menu, "启动 dsm_gm.mrp", CMD_DSM_GM, @"");
        g_recent_menu = [[NSMenu alloc] initWithTitle:@"最近打开 MRP"];
        NSMenuItem *recent_item = [[NSMenuItem alloc] initWithTitle:@"最近打开 MRP"
                                                             action:nil
                                                      keyEquivalent:@""];
        [recent_item setSubmenu:g_recent_menu];
        [file_menu addItem:recent_item];
        add_item(file_menu, "高级启动...", CMD_ADVANCED, @"");
        [file_menu addItem:[NSMenuItem separatorItem]];
        add_item(file_menu, "重启模拟器", CMD_RESTART, @"r");
        [file_item setSubmenu:file_menu];
        [menubar insertItem:file_item atIndex:insert_at];
        g_file_item = file_item;

        NSMenuItem *set_item = [[NSMenuItem alloc] initWithTitle:@"设置" action:nil keyEquivalent:@""];
        NSMenu *set_menu = [[NSMenu alloc] initWithTitle:@"设置"];
        add_item(set_menu, "设置屏幕分辨率...", CMD_SCREEN, @"");
        add_item(set_menu, "设置应用可见内存...", CMD_MEMORY, @"");
        add_item(set_menu, "设置应用可见设备日期...", CMD_DATE, @"");
        add_item(set_menu, "设置运行和 MRP 文件系统的工作目录...", CMD_WORKDIR, @"");
        add_item(set_menu, "设置域名替换规则...", CMD_DNS, @"");
        [set_item setSubmenu:set_menu];
        [menubar insertItem:set_item atIndex:insert_at + 1];
        g_settings_item = set_item;

        desktop_shell_cocoa_refresh_recents();
    }
    return 0;
}

void desktop_shell_cocoa_shutdown(void) {
    @autoreleasepool {
        NSMenu *menubar = [NSApp mainMenu];
        if (menubar && g_file_item) [menubar removeItem:g_file_item];
        if (menubar && g_settings_item) [menubar removeItem:g_settings_item];
        g_file_item = nil;
        g_settings_item = nil;
        g_recent_menu = nil;
        g_target = nil;
    }
}

void desktop_shell_cocoa_set_title(const char *title) {
    @autoreleasepool {
        NSString *s = ns_utf8(title ? title : "SkyEngine");
        NSWindow *key = [NSApp keyWindow];
        if (key) [key setTitle:s];
        for (NSWindow *win in [NSApp windows]) {
            [win setTitle:s];
        }
    }
}

void desktop_shell_cocoa_refresh_recents(void) {
    @autoreleasepool {
        if (!g_recent_menu) return;
        [g_recent_menu removeAllItems];
        int n = desktop_shell_recent_count();
        if (n <= 0) {
            NSMenuItem *empty = [[NSMenuItem alloc] initWithTitle:@"（空）"
                                                           action:nil
                                                    keyEquivalent:@""];
            [empty setEnabled:NO];
            [g_recent_menu addItem:empty];
            return;
        }
        int i;
        for (i = 0; i < n; i++) {
            const char *path = desktop_shell_recent_at(i);
            add_item(g_recent_menu, path ? path : "", CMD_RECENT_BASE + i, @"");
        }
    }
}

int desktop_shell_cocoa_pick_mrp(char *out, size_t n) {
    __block int rc = -1;
    void (^run)(void) = ^{
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setTitle:@"选择要启动的 MRP 文件"];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        [panel setAllowedFileTypes:@[@"mrp"]];
        if ([panel runModal] == NSModalResponseOK) {
            NSURL *url = [[panel URLs] firstObject];
            if (url) rc = copy_utf8([url path], out, n);
        }
    };
    if ([NSThread isMainThread]) run();
    else dispatch_sync(dispatch_get_main_queue(), run);
    return rc;
}

int desktop_shell_cocoa_pick_dir(char *out, size_t n) {
    __block int rc = -1;
    void (^run)(void) = ^{
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setTitle:@"设置运行和 MRP 文件系统的工作目录"];
        [panel setCanChooseFiles:NO];
        [panel setCanChooseDirectories:YES];
        [panel setAllowsMultipleSelection:NO];
        [panel setCanCreateDirectories:YES];
        if ([panel runModal] == NSModalResponseOK) {
            NSURL *url = [[panel URLs] firstObject];
            if (url) rc = copy_utf8([url path], out, n);
        }
    };
    if ([NSThread isMainThread]) run();
    else dispatch_sync(dispatch_get_main_queue(), run);
    return rc;
}

int desktop_shell_cocoa_advanced(char *mrp, size_t mrp_n, char *ext, size_t ext_n,
                                 char *entry, size_t entry_n) {
    __block int rc = -1;
    void (^run)(void) = ^{
        SkyEngineAdvancedPanel *panel = [[SkyEngineAdvancedPanel alloc] init];
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"高级启动"];
        [alert setInformativeText:@"选择 MRP，并指定 EXT_NAME 与 ENTRY。"];
        [alert addButtonWithTitle:@"确定"];
        [alert addButtonWithTitle:@"取消"];

        NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 480, 150)];
        NSTextField *mrp_label = [NSTextField labelWithString:@"选择要启动的 MRP 文件"];
        [mrp_label setFrame:NSMakeRect(0, 126, 480, 18)];
        NSTextField *mrp_field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 98, 370, 24)];
        [mrp_field setStringValue:ns_utf8(mrp && mrp[0] ? mrp : desktop_shell_current_mrp())];
        panel.mrpField = mrp_field;
        NSButton *browse = [[NSButton alloc] initWithFrame:NSMakeRect(380, 96, 100, 28)];
        [browse setTitle:@"浏览..."];
        [browse setBezelStyle:NSRoundedBezelStyle];
        [browse setTarget:panel];
        [browse setAction:@selector(browse:)];

        NSTextField *ext_label = [NSTextField labelWithString:@"EXT_NAME（VMRP_EXT 或 start.mr）"];
        [ext_label setFrame:NSMakeRect(0, 74, 480, 18)];
        NSComboBox *ext_box = [[NSComboBox alloc] initWithFrame:NSMakeRect(0, 50, 480, 24)];
        [ext_box addItemWithObjectValue:@"VMRP_EXT"];
        [ext_box addItemWithObjectValue:@"start.mr"];
        [ext_box setStringValue:ns_utf8(desktop_shell_last_ext())];

        NSTextField *entry_label = [NSTextField labelWithString:@"ENTRY（VMRP_ENTRY 或空）"];
        [entry_label setFrame:NSMakeRect(0, 26, 480, 18)];
        NSComboBox *entry_box = [[NSComboBox alloc] initWithFrame:NSMakeRect(0, 2, 480, 24)];
        [entry_box addItemWithObjectValue:@"VMRP_ENTRY"];
        [entry_box addItemWithObjectValue:@"（空）"];
        const char *last_entry = desktop_shell_last_entry();
        [entry_box setStringValue:ns_utf8(last_entry && last_entry[0] ? last_entry : "（空）")];

        [box addSubview:mrp_label];
        [box addSubview:mrp_field];
        [box addSubview:browse];
        [box addSubview:ext_label];
        [box addSubview:ext_box];
        [box addSubview:entry_label];
        [box addSubview:entry_box];
        [alert setAccessoryView:box];

        if ([alert runModal] == NSAlertFirstButtonReturn) {
            copy_utf8([mrp_field stringValue], mrp, mrp_n);
            copy_utf8([ext_box stringValue], ext, ext_n);
            copy_utf8([entry_box stringValue], entry, entry_n);
            rc = 0;
        }
        (void)panel;
    };
    if ([NSThread isMainThread]) run();
    else dispatch_sync(dispatch_get_main_queue(), run);
    return rc;
}

int desktop_shell_cocoa_prompt(const char *title, const char *label,
                               const char *initial, char *out, size_t n,
                               int multiline, const char **choices, int nchoices,
                               int list_only) {
    __block int rc = -1;
    void (^run)(void) = ^{
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:ns_utf8(title)];
        [alert setInformativeText:ns_utf8(label)];
        [alert addButtonWithTitle:@"确定"];
        [alert addButtonWithTitle:@"取消"];

        NSView *acc;
        NSComboBox *combo = nil;
        NSTextField *field = nil;
        NSScrollView *scroll = nil;
        NSTextView *view = nil;

        if (choices && nchoices > 0) {
            combo = [[NSComboBox alloc] initWithFrame:NSMakeRect(0, 0, 420, 26)];
            int i;
            for (i = 0; i < nchoices; i++) {
                [combo addItemWithObjectValue:ns_utf8(choices[i])];
            }
            [combo setStringValue:ns_utf8(initial)];
            if (list_only) [combo setCompletes:NO];
            acc = combo;
        } else if (multiline) {
            scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 420, 140)];
            [scroll setHasVerticalScroller:YES];
            [scroll setBorderType:NSBezelBorder];
            view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 140)];
            [view setString:ns_utf8(initial)];
            [scroll setDocumentView:view];
            acc = scroll;
        } else {
            field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 420, 24)];
            [field setStringValue:ns_utf8(initial)];
            acc = field;
        }
        [alert setAccessoryView:acc];
        if ([alert runModal] == NSAlertFirstButtonReturn) {
            if (combo) copy_utf8([combo stringValue], out, n);
            else if (view) copy_utf8([[view textStorage] string], out, n);
            else copy_utf8([field stringValue], out, n);
            rc = 0;
        }
    };
    if ([NSThread isMainThread]) run();
    else dispatch_sync(dispatch_get_main_queue(), run);
    return rc;
}
