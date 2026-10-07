/* Точка входа приложения: создаёт окно и показывает в нём страницу Index. */

import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x3200;
const TAG = 'WPEBrowser';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, 'приложение создано');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'страница не загрузилась: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, TAG, 'страница загружена');
    });
  }

  onWindowStageDestroy(): void {
    hilog.info(DOMAIN, TAG, 'окно закрыто');
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, 'приложение закрыто');
  }
}
