# Auditoria da instalação — 24/09/2026

Base: `34578498` (`origin/main`), versão publicada mais recente consultada
`v1.8.0-rc-2`. Alterações desta auditoria são locais e não publicadas.

## Resultado

Atualização após a auditoria: o usuário informou “instalei e foi superado”.
Isso registra uma instalação bem-sucedida e a superação do travamento no
cenário testado pelo usuário: **X4 Pro, versão 1.8.0**. Ele também confirmou
que conseguiu instalar **outro firmware pelo SD após usar a 1.8.0**. Não
atribuir esse resultado ao patch OTA local nem estendê-lo a todos os modelos. A condição exigida pelo usuário foi
satisfeita para retomar as melhorias de leitura.

Na inspeção inicial, não era possível certificar que o problema estivesse resolvido no
aparelho. A auditoria encontrou um caminho OTA ativo ainda usando apagamento
integral. A correção local elimina esse comportamento; validação física segue
pendente. Não foi detectado um leitor conectado por USB durante a auditoria.

## Caminhos diferentes

- SD: `SdFirmwareUpdateActivity::performUpdate` chama `flashFromSdPath`.
  O gravador apaga setores de 4 KiB, atende o watchdog, pausa logs persistentes
  e muda o destino de boot apenas após concluir a escrita.
- Navegador: `CrossPointWebServer::processPendingFirmwareInstall` valida o
  candidato e usa o mesmo gravador por setores.
- Menu OTA: `OtaUpdateActivity::runUpdateInstall` chama
  `OtaUpdater::installUpdate`, que **não** usa `flashFromSdPath`. Na base
  auditada, `esp_ota_begin` recebe o tamanho integral da imagem (ou
  `OTA_SIZE_UNKNOWN`), solicitando um apagamento longo antes de gravar dados.
  Portanto, a descrição antiga de um único gravador compartilhado era incorreta.

## Correção local

`OtaSectorWriter.h` inicia o SDK em `OTA_WITH_SEQUENTIAL_WRITES`, divide os
blocos recebidos nas fronteiras de setores e atende o watchdog antes e depois
de cada operação. Uma pausa permite também executar a tarefa idle. Não adiciona
buffers, tarefas, alocações ou desativação de watchdog. Os controles existentes
de digest, assinatura, aborto e seleção de boot permanecem no chamador.

## Evidência automatizada do patch inicial

O teste `test/ota_sector_writer/OtaSectorWriterTest.cpp` executa o helper de
produção com substitutos das chamadas de SDK e relógio. Dez cenários passaram
com AddressSanitizer e UndefinedBehaviorSanitizer: imagem de mais de 6 MiB,
cabeçalho inicial de 14 bytes, blocos de 1/14/4096/8192/65536 bytes, falhas no
primeiro/segundo/décimo setor, falha de inicialização e escrita vazia.

Isso demonstra limites das chamadas, conteúdo e progresso corretos, parada
após erro e atendimento ao watchdog. Não mede duração de flash real nem testa
o fluxo completo de rede, assinatura, bootloader ou instalação física.

A compilação completa `pio run -e x4-pro` passou, incluindo a verificação do
limite da partição OTA: imagem de 6.138.224 bytes para 8.257.536 bytes disponíveis.
SDK FreeInk fixado em `24003795381a6c23630a26472ae3b06550333e71`.
Esta compilação local identifica-se como `1.8.0-x4-pro`; não é uma release
assinada para distribuição. X3/X4 e Sticky não foram compilados nesta auditoria.

As compilações e testes das correções posteriores estão em
[update-corrections-validation.md](update-corrections-validation.md).

## Condição para certificar no aparelho

1. Identificar modelo, versão que executa o instalador e caminho que falhou.
2. Se o firmware antigo ainda falha no SD, usar um caminho de bootstrap ou
   recuperação comprovadamente funcional para esse aparelho. A imagem nova
   não pode corrigir o instalador antigo antes de ser instalada.
3. Com o instalador corrigido em execução, instalar uma imagem válida e
   assinada correspondente ao modelo pelo caminho que apresentou a falha.
4. Registrar progresso, causa de reset e versão após reiniciar; confirmar
   ausência de `TASK_WDT`/`INT_WDT` e integridade de notas, etiquetas e progresso.
5. Testar rejeição de candidato inválido e falha/cancelamento de download sem
   selecionar o candidato como próximo boot. Não provocar cortes de energia
   sem um procedimento de recuperação já confirmado.

## Comparação dos instaladores publicados

Inspeção direta dos arquivos `src/network/FirmwareFlasher.cpp`,
`src/network/OtaUpdater.cpp`, `src/activities/settings/SdFirmwareUpdateActivity.cpp`
e `src/main.cpp`, nas tags abaixo:

| Projeto/tag | Instalador SD | Menu OTA |
| --- | --- | --- |
| CrossInk `v1.6.0` | Apagamento de 64 KiB, escrita de 4 KiB, `delay(1)` | Baixa no SD, valida o arquivo aberto e usa o mesmo gravador |
| CrossPoint `1.6.5rc` | Apagamento de 64 KiB, escrita de 4 KiB, `delay(1)` | `esp_ota_begin(..., OTA_SIZE_UNKNOWN, ...)`, apagamento integral inicial |
| XPoint `v2.1.0` | Apagamento de 64 KiB, escrita de 4 KiB, `delay(1)` | `esp_ota_begin(..., OTA_SIZE_UNKNOWN, ...)`, apagamento integral inicial |
| YACP `v1.7.1-yacp` | Apagamento de 64 KiB, escrita de 4 KiB, reset de watchdog e pausa | `esp_ota_begin` com tamanho da imagem ou `OTA_SIZE_UNKNOWN` |

Fontes: [CrossInk](https://github.com/uxjulia/CrossInk/blob/v1.6.0/src/network/OtaUpdater.cpp),
[CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader/blob/1.6.5rc/src/network/OtaUpdater.cpp),
[XPoint](https://github.com/Belphemur/XPoint/blob/v2.1.0/src/network/OtaUpdater.cpp),
[YACP](https://github.com/Sichroteph/YACP/blob/v1.7.1-yacp/src/network/OtaUpdater.cpp).

Precisão sobre a regressão original: a tag INKacademic `v1.7.0-rc.2` já tinha
apagamento SD de 16 KiB e uma guarda temporária de watchdog de 60 segundos.
O código posterior reduz esse apagamento a 4 KiB. Portanto, não se deve dizer
que aquela versão fazia apagamento integral no SD: esse comportamento foi
encontrado no menu OTA, um caminho distinto.

O `main.cpp` dos forks consultados não contém a inicialização explícita de
watchdog da tarefa principal que o INKacademic adiciona. Isso não significa
que esses firmwares não tenham watchdogs: o SDK pode ativar outras proteções.
Sem comparar tarefas inscritas, configuração efetiva e tempos no aparelho,
o funcionamento de um fork não certifica outro.

Conclusão: as correções do INKacademic reduzem a janela entre serviços do
watchdog, mas a comparação não prova que a falha original foi eliminada.
O desenho do CrossInk (baixar/validar antes de gravar e compartilhar o gravador)
é uma referência útil para unificar nossos caminhos posteriormente, mantendo
setores de 4 KiB e assinaturas. Não importar o apagamento de 64 KiB como solução.

## Continuação autorizada

Com a confirmação do usuário, prosseguir com a incorporação de:
precisão das marcações, vínculos ao renomear, memória EPUB/fontes, correções de
iluminação, biblioteca com busca, filtros acadêmicos, fontes/menus maiores,
melhorias YACP de layout/dicionário, redes Wi-Fi salvas e correções de telas
aplicáveis ao hardware. Comparar cada mudança com a base antes de importar.
Páginas estáveis e motor TTF/OTF exigem avaliação própria de compatibilidade;
nenhum formato de anotação ou fonte deve ser substituído automaticamente.
