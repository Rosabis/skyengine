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
        add_item(set_menu, "设置运行模式...", CMD_PROFILE, @"");
        add_item(set_menu, "设置屏幕分辨率...", CMD_SCREEN, @"");
        add_item(set_menu, "设置应用可见内存...", CMD_MEMORY, @"");
        add_item(set_menu, "设置应用可见设备日期...", CMD_DATE, @"");
        add_item(set_menu, "设置运行和 MRP 文件系统的工作目录...", CMD_WORKDIR, @"");
        add_item(set_menu, "设置域名替换规则...", CMD_DNS, @"");
        add_item(set_menu, "选择 SoundFont (SF2)...", CMD_SF2, @"");
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

@interface SkyEngineDnsEditor : NSObject <NSTableViewDataSource>
@property (strong) NSMutableArray<NSString *> *lines;
@property (strong) NSTableView *table;
- (void)reloadRules:(DesktopDnsRule *)rules count:(int)count;
@end

@implementation SkyEngineDnsEditor
- (instancetype)init {
    self = [super init];
    if (self) {
        _lines = [NSMutableArray array];
    }
    return self;
}
- (void)reloadRules:(DesktopDnsRule *)rules count:(int)count {
    int i;
    [self.lines removeAllObjects];
    for (i = 0; i < count; i++) {
        char line[DESKTOP_DNS_HOST_MAX * 2 + 8];
        desktop_dns_format(&rules[i], line, sizeof(line));
        [self.lines addObject:ns_utf8(line)];
    }
    [self.table reloadData];
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView {
    (void)tableView;
    return (NSInteger)self.lines.count;
}
- (id)tableView:(NSTableView *)tableView objectValueForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    (void)tableView;
    (void)column;
    if (row < 0 || row >= (NSInteger)self.lines.count) return @"";
    return self.lines[(NSUInteger)row];
}
@end

int desktop_shell_cocoa_dns_editor(char *map, size_t n) {
    __block int rc = -1;
    void (^run)(void) = ^{
        DesktopDnsRule rules[DESKTOP_DNS_RULE_MAX];
        __block int count = desktop_dns_parse(map, rules, DESKTOP_DNS_RULE_MAX);
        SkyEngineDnsEditor *editor = [[SkyEngineDnsEditor alloc] init];
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"域名替换规则"];
        [alert setInformativeText:@"选择一条后可删除。"];
        [alert addButtonWithTitle:@"新增"];
        [alert addButtonWithTitle:@"删除"];
        [alert addButtonWithTitle:@"关闭"];

        NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 460, 200)];
        [scroll setHasVerticalScroller:YES];
        [scroll setBorderType:NSBezelBorder];
        NSTableView *table = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, 440, 200)];
        NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"rule"];
        [col setWidth:420];
        [col setTitle:@"现有替换"];
        [table addTableColumn:col];
        [table setHeaderView:nil];
        [table setDataSource:editor];
        editor.table = table;
        [editor reloadRules:rules count:count];
        [scroll setDocumentView:table];
        [alert setAccessoryView:scroll];

        for (;;) {
            NSModalResponse resp = [alert runModal];
            if (resp == NSAlertFirstButtonReturn) {
                NSAlert *pair = [[NSAlert alloc] init];
                [pair setMessageText:@"新增域名替换"];
                [pair addButtonWithTitle:@"确定"];
                [pair addButtonWithTitle:@"取消"];
                NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 360, 90)];
                NSTextField *from_l = [NSTextField labelWithString:@"被替换值"];
                [from_l setFrame:NSMakeRect(0, 66, 360, 18)];
                NSTextField *from_e = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 44, 360, 22)];
                NSTextField *to_l = [NSTextField labelWithString:@"替换值"];
                [to_l setFrame:NSMakeRect(0, 22, 360, 18)];
                NSTextField *to_e = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 360, 22)];
                [box addSubview:from_l];
                [box addSubview:from_e];
                [box addSubview:to_l];
                [box addSubview:to_e];
                [pair setAccessoryView:box];
                if ([pair runModal] == NSAlertFirstButtonReturn && count < DESKTOP_DNS_RULE_MAX) {
                    char from[DESKTOP_DNS_HOST_MAX];
                    char to[DESKTOP_DNS_HOST_MAX];
                    copy_utf8([from_e stringValue], from, sizeof(from));
                    copy_utf8([to_e stringValue], to, sizeof(to));
                    if (from[0] && to[0]) {
                        snprintf(rules[count].from, sizeof(rules[0].from), "%s", from);
                        snprintf(rules[count].to, sizeof(rules[0].to), "%s", to);
                        count++;
                        [editor reloadRules:rules count:count];
                    }
                }
                continue;
            }
            if (resp == NSAlertSecondButtonReturn) {
                NSInteger row = [table selectedRow];
                if (row >= 0 && row < count) {
                    int i = (int)row;
                    if (i + 1 < count) {
                        memmove(&rules[i], &rules[i + 1],
                                (size_t)(count - i - 1) * sizeof(rules[0]));
                    }
                    count--;
                    [editor reloadRules:rules count:count];
                }
                continue;
            }
            break;
        }
        rc = desktop_dns_serialize(rules, count, map, n);
        (void)editor;
    };
    if ([NSThread isMainThread]) run();
    else dispatch_sync(dispatch_get_main_queue(), run);
    return rc;
}

int desktop_shell_cocoa_pick_sf2(char *out, size_t n) {
    __block int rc = -1;
    void (^run)(void) = ^{
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setTitle:@"选择 SoundFont (SF2)"];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        [panel setAllowedFileTypes:@[@"sf2"]];
        if ([panel runModal] == NSModalResponseOK) {
            NSURL *url = [[panel URLs] firstObject];
            if (url) rc = copy_utf8([url path], out, n);
        }
    };
    if ([NSThread isMainThread]) run();
    else dispatch_sync(dispatch_get_main_queue(), run);
    return rc;
}
