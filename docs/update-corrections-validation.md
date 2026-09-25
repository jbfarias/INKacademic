# Correções do atualizador — 24/09/2026

Trabalho local sobre `34578498`, sem publicação ou instalação automática.
A auditoria da `v1.8.0-rc-2` descreve os binários antigos; estas mudanças não
alteram retroativamente uma versão já instalada.

## Comportamento implementado

| Caminho | Outros projetos | Verificações antes do próximo boot |
| --- | --- | --- |
| SD | Permitidos | Estrutura ESP, chip, tamanho, checksum/SHA e releitura da flash |
| Navegador — manual | Permitidos, sem `.sig` do INKademic | Mesmas verificações; digest do arquivo preparado é preservado até instalar |
| OTA — origem manual | Permitidos por URL HTTP(S) direta | Download limitado à partição, validação em SD, gravação por setores e releitura da flash |
| INKademic oficial — navegador/menu OTA | Chave oficial obrigatória | Assinatura Ed25519, identidade/modelo, versão mais nova, integridade e releitura |

A opção manual é uma escolha explícita. Uma assinatura oficial ausente ou
inválida não faz o atualizador passar automaticamente para o modo manual.
Arquivos manuais não precisam usar nossa chave ou marcador de identidade, e
sua versão pode seguir outra numeração. A compatibilidade do chip não identifica
o modelo de placa de todos os projetos: o usuário escolhe o arquivo do modelo
correto. O texto da interface não apresenta arquivos manuais como autenticados.

Na página **Firmware** do leitor:

1. Para arquivo local, escolher **Manual image / another project** e enviar o
   `.bin`. Após a validação, usar **Install and reboot**.
2. Para OTA direto, informar a URL em **Manual OTA / another project**, baixar
   para o leitor e instalar após a validação.
3. Para usar a URL no menu OTA do aparelho, clicar em **Use this link in the
   device OTA menu**. O aparelho identifica a origem manual e pede confirmação.
4. **Use official source in OTA menu** restaura a origem oficial. A página
   continua oferecendo o catálogo oficial mesmo quando há uma URL manual salva.

A origem salva fica em `/.inkademic-ota-source`, limitada a 1024 bytes. Uma
configuração inválida gera erro; não inicia instalação. O OTA manual mantém a
verificação de certificados HTTPS do transporte padrão. Não usa a alternativa
wolfSSL sem certificados de CA, reservada ao fluxo com digest confiável.

## Correções da auditoria

- Ed25519 e a política do fluxo oficial agora são habilitados em todos os
  ambientes de hardware de aplicação. Ausência de definições provoca erro;
  uma checagem após o link exige chave pública e funções reais no binário.
- O menu OTA oficial usa escrita sequencial por setores de 4 KiB, incluindo
  blocos desalinhados, com atendimento ao watchdog e pausas para a tarefa idle.
- Identidade é lida com um parser limitado, sem deslocar uma string a cada byte.
  Identidade, assinatura e estrutura são vinculadas aos mesmos bytes.
- O gravador relê a flash antes da seleção de boot; falhas e alterações entre
  validação e escrita não autorizam a reinicialização no candidato.
- Pedidos multipart recusados não ganham controle sobre a instalação pendente.
  Respostas consideram o pedido atual. A confirmação inclui modo e digest do
  candidato mostrado, recusando confirmações antigas após substituição do arquivo. Valores inválidos de tamanho, sessão e
  offset são recusados antes de modificar arquivos.
- Ações de firmware exigem cabeçalho explícito e, em navegadores, a origem do
  próprio leitor. Isso não implementa autenticação de usuários na rede.
- A identificação de RC é consistente entre os modelos; a comparação não
  estoura inteiros e diferencia desenvolvimento, RC e estável.
- Imagens invertidas usam a mesma transformação por pixel que as normais.
- A documentação da luz informa o comportamento real: default apenas quando
  não há valor salvo; zeros antigos permanecem, sem presumir a intenção do usuário.
- 21 binários gerados deixaram de ser rastreados no índice Git. O histórico e os
  arquivos das releases não foram reescritos.

## Chaves e rotação

A chave atual foi preservada. Não há evidência de comprometimento nesta análise
nem motivo para aceitar uma chave antiga adicional. A instalação manual permite
migrar entre projetos ou chaves sem depender de uma assinatura do INKademic.

Para uma futura rotação automática oficial, será necessário preparar uma versão
ponte assinada pela chave ainda confiável, com a nova chave incorporada, testar
as assinaturas de transição e só depois remover a confiança antiga. Em caso de
comprometimento, aceitar a chave antiga não constitui uma correção. Este trabalho
não publica uma rotação nem afirma que uma transição automática já existe.

## Validação automatizada

A suíte em `test/firmware_security` compila o gravador e o OTA manual de produção
com armazenamento, rede e flash substituídos por implementações determinísticas.
Ela mantém SHA-256 real e o verificador Ed25519 do mesmo Arduino-wolfSSL 5.7.2
usado no firmware. Não usa uma chave privada nem uma verificação simulada.

- Assinaturas públicas das três imagens da rc-2: válidas, alteradas e truncadas.
- Firmware sem assinatura/identidade do INKademic: aceito em SD, navegador manual
  e OTA manual. O fluxo oficial continua rejeitando esse mesmo tipo de arquivo.
- Chip errado, arquivo corrompido, download cancelado/maior que a partição,
  falha de rede, arquivo substituído e falhas de leitura/escrita/apagamento.
- Origem manual salva, retorno à origem oficial, versões e parâmetros inválidos.
- Limites de setor e atendimento ao watchdog; quatro orientações de imagem.
- JavaScript real da página com transporte simulado: modos, assinatura, URL,
  origem salva e respostas de erro.
- Teste adicional com o `.bin` publicado completo do X4 Pro: assinatura válida,
  gravação/releitura e recusa de arquivo substituído após a validação.

ASan e UBSan estão ativos no código da aplicação. Apenas a checagem de
`shift-base` fica suprimida na implementação ref10 de terceiros do wolfSSL,
que usa deslocamentos de valores com sinal; os demais sanitizadores permanecem.
O teste de pedidos HTTP verifica a política de posse/origem e o JavaScript,
mas não executa um servidor WebServer real: esse fluxo ainda requer teste no leitor.

Os cinco grupos de testes passaram; o teste adicional com o binário publicado
completo do X4 Pro também passou. A análise estática direcionada aos oito
componentes do atualizador, com as definições de hardware, não relatou defeitos.
A formatação dos arquivos alterados e a sintaxe de Python/JavaScript passaram.

O teste da página também confere as rotas e os handlers registrados no servidor.
Essa checagem foi adicionada após detectar, antes da entrega, que faltava ligar
os novos endpoints de OTA manual. A presença dessas rotas e da proteção da
confirmação foi conferida novamente nos três binários finais.

As três compilações finais de hardware passaram, incluindo a checagem da chave
e dos símbolos Ed25519. Todos os artefatos abaixo incluem os últimos ajustes.

| Artefato | Bytes | Partição OTA | Margem |
| --- | ---: | ---: | ---: |
| x3-x4 | 6315168 | 6553600 | 238432 |
| sticky | 6108368 | 6553600 | 445232 |
| x4-pro | 6215744 | 8257536 | 2041792 |

Arquivos de teste e hashes: `dist-publish/update-corrections-2026-09-24/`.
Identificam-se como builds `1.8.0-dev+...`, sem assinatura de release. Não são
uma nova release publicada. A instalação inicial pelo SD evita depender do
verificador de rede da versão antiga. Nenhuma compilação foi gravada em um
aparelho nesta etapa.

## Teste físico

O usuário confirmou **X4 Pro, versão 1.8.0**, e a instalação de **outro firmware
pelo SD após a 1.8.0**. Isso confirma aquele cenário; não valida as alterações
locais posteriores nem os demais modelos.

No X4 Pro, após instalar esta compilação de desenvolvimento pelo SD, verificar:

1. Um firmware correto de outro projeto pelo navegador em modo manual e, em
   outro ciclo, por URL OTA; ambos devem chegar à confirmação e iniciar o
   firmware escolhido sem travamento.
2. Origem salva no navegador aparecendo como manual no menu OTA; opção de
   restauração voltando ao catálogo oficial.
3. `.bin`/`.sig` oficial válido aceito; assinatura alterada ou ausente recusada
   no modo assinado. Escolher uma versão oficial realmente mais nova que a
   compilação em execução, ou usar modo manual para uma troca intencional.
4. Upload interrompido permitindo retomada; pedido inválido preservando o
   arquivo pendente; tela indicando falha quando a instalação não é iniciada.
5. Logo invertido em retrato/paisagem e preferência de luz preservada ao acordar.

Confirmar versão após reiniciar, ausência de resets de watchdog e preservação
ou compatibilidade dos dados conforme o firmware de destino. Repetir nos demais
modelos antes de afirmar validação física para X3/X4 e Sticky. Nenhum teste de
host mede o tempo real de flash ou certifica o bootloader.
