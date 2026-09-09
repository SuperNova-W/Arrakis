import modelManifest from '../../backend/deploy/news_models/XLK.json.manifest.json'
import modelMetrics from '../../backend/deploy/news_models/XLK.json.metrics.json'
import finbertManifest from '../../backend/models/finbert/manifest.json'

const articleFeatureStart = modelManifest.feature_names.indexOf('article_count')
const embeddingFeatureStart = modelManifest.feature_names.findIndex(name => name.startsWith('embedding_'))

/**
 * User-facing model-card data sourced from the versioned deployment artifacts.
 * Keep the artifact field names out of components so copy stays readable while
 * the numbers remain tied to the checked-in model and evaluation files.
 */
export const XLK_SIGNAL_MODEL = {
  signalModel: 'XGBoost',
  languageModel: 'FinBERT',
  languageModelVersion: finbertManifest.artifact_id.replace(/^arrakis-finbert-/, ''),
  featureNames: modelManifest.feature_names,
  marketFeatureCount: articleFeatureStart,
  newsFeatureCount: modelManifest.feature_names.length - articleFeatureStart,
  textRepresentationCount: embeddingFeatureStart >= 0 ? modelManifest.feature_names.length - embeddingFeatureStart : 0,
  trainingRows: modelMetrics.train_rows,
  validationRows: modelMetrics.validation_rows,
  testRows: modelMetrics.test_rows,
  validationAuc: modelMetrics.validation.roc_auc,
  validationAccuracy: modelMetrics.validation.accuracy,
  validationLogLoss: modelMetrics.validation.log_loss,
  testAuc: modelMetrics.test.roc_auc,
  testAccuracy: modelMetrics.test.accuracy,
  testLogLoss: modelMetrics.test.log_loss,
  validationStart: modelMetrics.validation_start,
  validationEnd: modelMetrics.validation_end,
  testStart: modelMetrics.test_start,
  testEnd: modelMetrics.test_end,
  selectedIteration: modelMetrics.selected_best_iteration,
  trainingRounds: modelMetrics.rounds_run,
} as const

