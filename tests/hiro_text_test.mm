#import <Cocoa/Cocoa.h>
#undef BSD
#include <hiro/hiro.hpp>
#include <cassert>
#include <cmath>
static NSTextView* findText(NSView* root) {
  if([root isKindOfClass:[NSTextView class]])return (NSTextView*)root;
  for(NSView* child in [root subviews])if(auto found=findText(child))return found;
  return nil;
}
int main() {
  hiro::Application::initialize();
  {
    hiro::Window window;hiro::VerticalLayout layout{&window};hiro::TextEdit text{&layout,hiro::Size{~0,~0}};
    window.setSize({640,560}).setVisible();text.setEditable(false);
    nall::string log;for(int i=0;i<100;i++)log.append("Log line ",i," with wrapped diagnostic text\n");text.setText(log);
    hiro::Application::processEvents();
    NSTextView* native=nil;for(NSWindow* candidate in [NSApp windows])if((native=findText([candidate contentView])))break;
    assert(native && [native frame].size.width>100 && [[native string] length]>100);
    auto scroll=[native enclosingScrollView];assert(scroll);
    [native setSelectedRange:NSMakeRange(4,7)];[[scroll contentView] scrollToPoint:NSMakePoint(0,120)];[scroll reflectScrolledClipView:[scroll contentView]];
    CGFloat position=[[scroll contentView] bounds].origin.y;
    log.append("New event\n");text.setText(log);hiro::Application::processEvents();
    assert([native selectedRange].location==4 && [native selectedRange].length==7);
    assert(std::abs([[scroll contentView] bounds].origin.y-position)<2);
    text.setText("Short log\n");hiro::Application::processEvents();
    assert([[scroll contentView] bounds].origin.y<2 && [native frame].size.width>100);
    window.setVisible(false);
  }
  hiro::Application::quit();puts("Native wrapped log width, selection, scroll preservation and short-log recovery passed.");
}
