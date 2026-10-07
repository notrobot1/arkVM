/*
 * Внешность обозревателя.
 *
 * Пока здесь один XComponent во всё окно — в него и рисует движок.
 * Строка адреса сверху нужна только чтобы было чем проверить передачу
 * ссылки из ArkTS в родную часть. Настоящую внешность возьмём позже
 * из applications/browser.
 *
 * libraryname: 'wpe' — по нему ArkUI находит libwpe.so и передаёт ей
 * XComponent при загрузке.
 */

import wpe from 'libwpe.so';

@Entry
@Component
struct Index {
  @State address: string = 'https://wpewebkit.org';

  build() {
    Column() {
      Row() {
        TextInput({ text: this.address })
          .layoutWeight(1)
          .height(40)
          .onChange((value: string) => { this.address = value; })
          .onSubmit(() => { wpe.loadUrl(this.address); });

        Button('Открыть')
          .height(40)
          .margin({ left: 8 })
          .onClick(() => { wpe.loadUrl(this.address); });
      }
      .width('100%')
      .padding(8);

      XComponent({
        id: 'wpeSurface',
        type: XComponentType.SURFACE,
        libraryname: 'wpe'
      })
        .width('100%')
        .layoutWeight(1);
    }
    .width('100%')
    .height('100%');
  }
}
