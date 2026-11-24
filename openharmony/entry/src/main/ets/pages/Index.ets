import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

@Entry
@Component
struct Index {
  onPageShow(): void {
    this.hideStatusBar();
  }

  private async hideStatusBar(): Promise<void> {
    try {
      const context = this.getUIContext().getHostContext();
      if (!context) {
        return;
      }
      const appWindow = await window.getLastWindow(context);
      await appWindow.setWindowLayoutFullScreen(true);
      await appWindow.setWindowSystemBarEnable([]);
    } catch (error) {
      hilog.warn(0x0000, 'MikanEngine', 'Could not hide system bars: %{public}s', String(error));
    }
  }

  build() {
    Column() {
      XComponent({ id: 'SDL_XComponent', type: XComponentType.SURFACE, libraryname: 'SDL3' })
        .id('SDL_XComponent')
        .width('100%')
        .height('100%')
    }
    .width('100%')
    .height('100%')
  }
}
