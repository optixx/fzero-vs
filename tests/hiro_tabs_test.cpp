#include <hiro/hiro.hpp>
#include <cassert>
#include <cstdio>
int main() {
  hiro::Application::initialize();
  for(int iteration=0;iteration<50;iteration++) {
    hiro::Window window;
    hiro::VerticalLayout layout{&window};
    hiro::TabFrame tabs{&layout,hiro::Size{~0,~0}};
    hiro::TabFrameItem host{&tabs},join{&tabs},diagnostics{&tabs};
    host.setText("Host");join.setText("Join");diagnostics.setText("Diagnostics");
    join.setSelected();
    tabs.remove(join);
    assert(tabs.itemCount()==2 && diagnostics.offset()==1 && join.offset()==-1);
    tabs.append(join);assert(join.offset()==2);
    if(iteration%2==0) {tabs.reset();assert(tabs.itemCount()==0);}
    // Alternate explicit reset and destruction with live tabs, as on app exit.
  }
  hiro::Application::quit();
  puts("Tab removal, reattachment, reset and window destruction passed (50 cycles).");
}
