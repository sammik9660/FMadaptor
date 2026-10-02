# Hardware

배선, 하드웨어 구성과 향후 회로 정보를 정리하는 위치입니다.

현재 hardware-verified 구성은 Waveshare RP2040-Zero + SI4703, RESET GPIO2 / SDA GPIO4 / SCL GPIO5, I2C 100 kHz입니다. SI4703의 3.5 mm 이어폰은 analog audio와 안테나 역할을 합니다. Samsung phone/app이 USB-C를 통해 tuner를 제어합니다. 검증 commit은 `a15fce261708a2150e12d6dc011ce3794606c2b7`입니다.

`rx.setup(RESET_PIN, SDA_PIN)`의 두 번째 인자는 실제 SDA입니다. Wire 핀/클록 지정 후 rx.setup을 호출하고 선행 Wire.begin은 두지 않습니다. 라이브러리가 SDA LOW → reset → 내부 Wire.begin → powerUp을 수행하도록 유지합니다.

별도 진단에서 GPIO8/9의 ACK 실패와 GPIO4/5의 반복 성공을 관찰했지만 실패 원인을 확정하지 않습니다. module 모델, supply wiring과 complete schematic은 추가 문서화가 필요합니다. 원본 사진/회로 자료는 아직 반입하지 않았습니다.

비정상 초기화 중 loud audio transient 사례가 있으므로 연결/전원 실험은 이어폰을 귀에서 뺀 상태로 진행합니다. 정상 검증 결과는 모든 power/error 상태의 무발생 보장이 아닙니다.
