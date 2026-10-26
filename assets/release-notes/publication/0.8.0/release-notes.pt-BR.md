# GameHQ 0.8.0 (2026-10-26)

## Agradecimentos especiais

- Um grande obrigado a [@galcn10](https://github.com/galcn10), criador do [Console Overlay for Playnite](https://github.com/galcn10/ConsoleOverlay), por testar o GameHQ, compartilhar feedback valioso e criar uma ótima integração com o Playnite. Seu apoio ajudou a melhorar o GameHQ 0.8.0!

## Destaques

- Detecção automática de atualizações aprimorada. O GameHQ agora procura novas versões logo após cada inicialização e a cada hora enquanto está em execução, para que você descubra as atualizações muito antes.
- Os clipes podem gravar o som do próprio jogo. Com a nova fonte Áudio do jogo, o GameHQ grava o jogo em qualquer dispositivo pelo qual ele toque (fones de ouvido, Bluetooth, HDMI ou alto-falantes) e deixa de fora outros aplicativos, como o Discord ou o Spotify.
- Problemas na gravação de áudio agora são detectados e informados. Se a fonte de áudio selecionada não puder ser gravada, o buffer de replay para e explica o motivo e qual fonte escolher no lugar, em vez de salvar clipes sem som sem avisar. O GameHQ nunca troca a fonte de áudio por conta própria.
- Som e imagem ficam sincronizados. Clipes salvos depois de uma tela de carregamento, uma pausa ou um travamento longo mantêm o tempo correto, e o buffer de replay reinicia sozinho quando um jogo muda de resolução.
- Capturas de tela em HDR não travam mais. Uma captura de tela de um jogo pausado ou de um menu estático em HDR é salva na hora. Se nenhum quadro chegar, a tentativa falha de forma limpa após alguns segundos e o próximo acionamento funciona.
- Exclusão mais segura com o controle. Na confirmação de exclusão, Cruz agora pressiona o botão destacado e Círculo cancela. Antes, Cruz podia excluir o arquivo mesmo com Cancelar destacado.
- Excluir protege seus originais. Ao excluir uma captura criada pelo GameHQ, ela agora vai para a Lixeira, para que você possa restaurá-la. Os arquivos das pastas monitoradas nunca são excluídos.

## Novidades

- Configurações › Replay › Fonte de áudio oferece Áudio do jogo, Áudio da área de trabalho e Desativado. Áudio do jogo é o padrão no Windows 11. Áudio da área de trabalho grava tudo na saída padrão do Windows, incluindo outros aplicativos, e é o padrão em versões mais antigas do Windows.
- Áudio do jogo usa a captura de áudio por aplicativo do próprio Windows e nunca mexe no processo do jogo. Se você desativou o áudio do sistema de propósito em uma versão anterior, ele continua desativado.
- Atualizações beta opcionais: a nova opção Incluir versões beta em Configurações › Geral e em Sobre permite que o atualizador ofereça versões beta. Ela vem desativada por padrão, então instalações estáveis recebem apenas atualizações estáveis.
- Quando o GameHQ falha, ele tenta salvar um pequeno despejo de memória na pasta de logs para ajudar a diagnosticar o problema. Nada é enviado. Algumas falhas, como as que o Windows encerra imediatamente, ainda podem fechar o GameHQ sem gerar um despejo.
- O log registra mais detalhes sobre capturas de tela salvas e falhas na inicialização, então um único arquivo de log basta com mais frequência para entender um relato de problema.

## Correções e melhorias

- Os clipes não ficam mais sem som por causa de uma configuração interna antiga que deixava o áudio da área de trabalho desativado por padrão.
- O áudio em qualquer formato de reprodução do Windows é convertido corretamente, em vez de a faixa de áudio ser perdida.
- Momentos silenciosos são gravados como silêncio, então o som depois de um trecho silencioso continua alinhado com a imagem.
- A gravação do áudio do jogo reinicia com mais confiabilidade quando um jogo é reiniciado ou quando você troca de jogo.
- Partes do buffer de replay que não puderam ser salvas ou não podem ser lidas de volta são deixadas de fora, para que não estraguem um clipe salvo.
- As notificações do próprio GameHQ, como o aviso de que uma captura de tela foi salva, não aparecem mais nas suas capturas de tela nem em gravações e transmissões da tela inteira.
- Uma captura de tela substituída por outra imagem com o mesmo nome de arquivo ganha uma miniatura nova na próxima verificação da biblioteca.
- As pastas de captura são verificadas em segundo plano, então o GameHQ continua respondendo bem com coleções grandes.
- Navegadores, editores e outras janelas que não são jogos não aparecem mais como jogos na sua biblioteca.
- Durante uma exclusão em massa, a seleção fica fixa enquanto a confirmação está aberta, então ações do controle em segundo plano não podem mudar quais arquivos serão excluídos.
- A caixa de diálogo de exclusão da sobreposição recebe a entrada do controle enquanto está aberta e fecha quando a sobreposição é ocultada.
- A sobreposição ignora janelas que pararam de responder em vez de esperar por elas.
- A tela Configurações não volta mais ao topo quando você rola com o mouse depois de voltar ao GameHQ.
- Escolher Padrões integrados ou remover uma predefinição no editor de mapeamento do controle não trava mais a seleção de predefinição.

## Controles e Playnite

- Os atalhos do controle continuam funcionando em jogos iniciados fora da Steam, como um jogo local aberto pelo Playnite. Quando o Windows deixa de entregar ao GameHQ um DualSense conectado enquanto o jogo está em foco, o GameHQ lê o controle diretamente, somente para leitura, e volta à entrada normal assim que o Windows volta a entregá-lo.
- Atalhos e repetições de botão mantido pressionado sempre terminam quando o mesmo pressionamento é solto, inclusive quando a liberação é informada pelo GameInput e quando o controle é desconectado com um botão pressionado.
- Os controles desconectados de vez são limpos, a verificação do HidHide não deixa mais a interface lenta e os dispositivos GameInput só são verificados de novo quando algo muda.
- Os jogos do Playnite continuam sendo reconhecidos mesmo quando o Playnite não consegue identificar o processo do jogo.
- Experiência de tela cheia do Xbox no Windows: quando a interface do Xbox está em primeiro plano, o GameHQ usa o jogo iniciado pelo Playnite para capturas de tela e clipes. O GameHQ Playnite Integration 0.4.13, disponível separadamente como pré-lançamento, também inicia o GameHQ quando o modo Xbox atrasa a inicialização dele ao entrar na conta.

## Estabilidade e confiabilidade

- A limpeza de gravações não concluídas remove apenas arquivos temporários criados pelo próprio GameHQ.
- As pastas de captura dos jogos sempre ficam dentro da sua pasta de capturas.
- A inicialização não falha mais quando a biblioteca contém caminhos de pasta duplicados; em vez disso, as entradas são mescladas.
- Se o GameHQ não conseguir iniciar, ele explica o motivo no seu idioma e registra o motivo no log.
- Importar um perfil portátil para um GameHQ instalado agora funciona quando a cópia instalada já foi iniciada.
- Segurança reforçada: carregamento de DLL mais seguro, verificações mais rigorosas nas conexões de integração locais e tratamento mais seguro dos arquivos de atualização.
- O atualizador lida melhor com uma primeira inicialização lenta e mantém seus registros intactos se for interrompido.
- O compartilhamento pelo Telegram é encerrado de forma mais segura.

## Instalação e atualização

- Atualizando a partir da 0.7.9: execute o instalador da 0.8.0 sobre a instalação atual.
- Instalador: instala para o usuário atual e substitui uma 0.7.9 instalada ou uma beta anterior.
- Portátil: extraia o ZIP em uma pasta nova e execute o GameHQ.exe.
- Se você testou a 0.8.0 Beta 2, 3 ou 4, instale a 0.8.0 com o instalador ou um ZIP portátil novo, porque essas betas não foram oferecidas pelo atualizador.

## Limitações conhecidas

- Áudio do jogo requer o Windows 11. Em versões mais antigas do Windows, o GameHQ usa Áudio da área de trabalho, que também grava outros aplicativos tocando na saída padrão.
- Um jogo que assume o uso exclusivo de um controle ainda pode impedir os atalhos em segundo plano.
- O suporte à experiência de tela cheia do Xbox no Windows ainda não foi confirmado por testadores em um PC que a utilize.
- Os binários para Windows não são assinados com Authenticode; o Windows SmartScreen pode mostrar um aviso de Editor desconhecido.
